// TUN mode (0.3.0): mihomo takes over the Frame's normal traffic (wlan0) through a tun device, like DeckyClash,
// but without a root daemon. The capability comes from a one-time, user-run step (tun-setup.sh, needs sudo):
// a root-owned copy of mihomo in /home/.frameclash with cap_net_admin — the pattern eyecam uses on the Frame.
//
// Unlike the Steam Deck, the Frame has a second Wi-Fi (wlanap, its own AP for PC streaming) and usb0: those and
// every private range stay out of the tun, auto-redirect (nftables) stays off so firewalld is never touched, and
// stop is SIGTERM only, which makes mihomo remove its routes and ip rules.
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// tunDir/tunBinary: root-owned, so a steamos process can't swap the capable binary.
func tunDir(home string) string    { return filepath.Join(filepath.Dir(home), ".frameclash") }
func tunBinary(home string) string { return filepath.Join(tunDir(home), "mihomo") }

const capNetAdmin = 12

// keep in sync with pkg/tun-setup.sh (cleanup)
const (
	tunDevice          = "frameclash0"
	tunTable           = 2022
	tunRuleIndex       = 9000
	tunDNSServer       = "198.18.0.2" // inside the tun's /30: dns-hijack answers it
	tunDNSNoPermission = "no-permission"
)

// tunReady checks the privileged copy: root-owned dir and file, not writable by others, not a symlink, and
// carrying cap_net_admin in its permitted+effective set.
func tunReady(home string) error {
	const setup = "TUN is not set up: run ~/.local/lib/frameclash/tun-setup.sh install (asks for your sudo password once)"
	for _, p := range []string{tunDir(home), tunBinary(home)} {
		st, err := os.Lstat(p)
		if err != nil {
			return errors.New(setup)
		}
		sys, ok := st.Sys().(*syscall.Stat_t)
		if !ok || sys.Uid != 0 || st.Mode().Perm()&0o022 != 0 || st.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("%s must be owned by root and not writable by others (re-run tun-setup.sh install)", p)
		}
	}
	buf := make([]byte, 64)
	n, err := syscall.Getxattr(tunBinary(home), "security.capability", buf)
	// vfs_cap_data: le32 magic_etc (bit 0 = effective), then le32 permitted (caps 0-31)
	if err != nil || n < 8 || binary.LittleEndian.Uint32(buf[0:4])&1 == 0 ||
		binary.LittleEndian.Uint32(buf[4:8])&(1<<capNetAdmin) == 0 {
		return fmt.Errorf("%s has no cap_net_admin (re-run tun-setup.sh install)", tunBinary(home))
	}
	return nil
}

// Never routed into the tun: LAN, the streaming AP's subnet, link-local, multicast.
var tunExcludeAddr = []any{
	"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10", "169.254.0.0/16", "224.0.0.0/4",
	"255.255.255.255/32", "fc00::/7", "fe80::/10", "ff00::/8",
}

// fake-ip must not answer for local names or STUN (DeckyClash's fake-ip-filter)
var tunFakeIPFilter = []any{"*.lan", "*.localdomain", "*.localhost", "*.local", "*.home.arpa",
	"stun.*.*", "stun.*.*.*", "+.stun.*.*", "+.stun.*.*.*", "+.stun.*.*.*.*"}

// applyTun edits a rendered config map for TUN mode.
func applyTun(cfg map[string]any) {
	cfg["tun"] = map[string]any{
		"enable":                true,
		"device":                tunDevice, // fixed names/numbers: tun-setup.sh cleanup removes exactly these
		"iproute2-table-index":  tunTable,
		"iproute2-rule-index":   tunRuleIndex,
		"stack":                 "mixed",
		"auto-route":            true,
		"auto-redirect":         false, // no nftables: firewalld keeps sole ownership of the ruleset
		"auto-detect-interface": true,  // mihomo's own traffic leaves via the default route (wlan0), never wlanap
		"strict-route":          false,
		"dns-hijack":            []any{"any:53"},
		"route-exclude-address": tunExcludeAddr,
		"exclude-interface":     []any{"wlanap", "usb0"},
	}
	// hijacked DNS needs mihomo's DNS on; keep the profile's servers, else DeckyClash's defaults
	dns, _ := cfg["dns"].(map[string]any)
	if dns == nil {
		dns = map[string]any{
			"ipv6":               false,
			"default-nameserver": []any{"223.5.5.5", "119.29.29.29"},
			"nameserver":         []any{"https://doh.pub/dns-query", "https://dns.alidns.com/dns-query"},
		}
	}
	dns["enable"] = true
	if dns["enhanced-mode"] == "fake-ip" {
		have := map[any]bool{}
		filter, _ := dns["fake-ip-filter"].([]any)
		for _, f := range filter {
			have[f] = true
		}
		for _, f := range tunFakeIPFilter {
			if !have[f] {
				filter = append(filter, f)
			}
		}
		dns["fake-ip-filter"] = filter
	}
	cfg["dns"] = dns
	// rules match domains even for connections made to IPs (DNS answered outside the tun)
	if _, ok := cfg["sniffer"]; !ok {
		cfg["sniffer"] = map[string]any{"enable": true, "sniff": map[string]any{
			"HTTP": map[string]any{"ports": []any{80, "8080-8880"}, "override-destination": true},
			"TLS":  map[string]any{"ports": []any{443, 8443}},
			"QUIC": map[string]any{"ports": []any{443, 8443}},
		}}
	}
}

func (a *App) hTun(w http.ResponseWriter, r *http.Request) {
	var in struct{ Enable bool }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	if in.Enable {
		if err := tunReady(a.home); err != nil {
			errOut(w, err)
			return
		}
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	a.state.TUN = in.Enable
	a.saveState()
	if err := a.restartIfRunning(); err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]bool{"ok": true})
}

// waitTun: mihomo creates the device within a second or two of starting.
func waitTun(done <-chan struct{}) bool {
	for i := 0; i < 50; i++ {
		if _, err := os.Stat("/sys/class/net/" + tunDevice); err == nil {
			return true
		}
		select {
		case <-done:
			return false
		case <-time.After(200 * time.Millisecond):
		}
	}
	return false
}

// setTunDNS points systemd-resolved into the tun ("self DNS"): resolved sends its queries bound to wlan0, which
// skips the tun's routing rules, so dns-hijack never saw them (poisoned answers instead of fake-ip). "~." on
// frameclash0 outranks wlan0's plain default route. Nothing is set on wlan0: when the tun goes away these settings
// go with it and resolution falls back to wlan0 by itself. Needs the polkit rule tun-setup.sh install adds
// (install --no-self-dns leaves it out). Off clears them again (same two actions).
// Returns the state for /api/status: "on", "off", tunDNSNoPermission or an error.
func setTunDNS(on bool) string {
	server, domain, state := tunDNSServer, "~.", "on"
	if !on {
		server, domain, state = "", "", "off" // an empty argument resets the link's setting
	}
	for _, args := range [][]string{{"dns", tunDevice, server}, {"domain", tunDevice, domain}} {
		out, err := exec.Command("resolvectl", append([]string{"--no-ask-password"}, args...)...).CombinedOutput()
		if err != nil {
			if strings.Contains(string(out), "authentication required") {
				log.Print("TUN DNS: no permission to change systemd-resolved, system DNS left as is " +
					"(tun-setup.sh install adds it; --no-self-dns leaves it out)")
				return tunDNSNoPermission
			}
			msg := fmt.Sprintf("TUN DNS: resolvectl %s: %s", args[0], strings.TrimSpace(string(out)))
			log.Print(msg)
			return msg
		}
	}
	log.Printf("TUN DNS %s (%s on %s)", state, tunDNSServer, tunDevice)
	return state
}

func (a *App) hTunDNS(w http.ResponseWriter, r *http.Request) {
	var in struct{ Enable bool }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	a.state.NoSelfDNS = !in.Enable
	a.saveState()
	if a.state.TUN && a.running() {
		a.tunDNS = setTunDNS(in.Enable)
		if a.tunDNS != "on" && a.tunDNS != "off" {
			msg := a.tunDNS
			if msg == tunDNSNoPermission {
				msg = "no permission to change the system DNS: run tun-setup.sh install"
			}
			errOut(w, errors.New(msg))
			return
		}
	}
	jsonOut(w, 200, map[string]bool{"ok": true})
}
