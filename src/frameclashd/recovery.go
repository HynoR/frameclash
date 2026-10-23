// Recovery (0.5.0): user-driven, no background monitoring. Measured on the Frame: when mihomo dies (crash,
// SIGKILL, OOM, suspend gone wrong) the kernel deletes the tun device and table 2022's routes; the leftover ip
// rules point at an empty table, so traffic falls back to wlan0 (fail-open). A reboot or power loss clears all
// TUN state. So the network is never left black-holed; pressing Start brings the proxy back, and
// "Repair network" (panel, `frameclash repair`, POST /api/repair) removes leftovers without root.
package main

import (
	"bytes"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"gopkg.in/yaml.v3"
)

// stopOrphans: a mihomo left from a previous frameclashd (it crashed or was killed). The setcap TUN copy gets no
// parent-death signal (the kernel clears it on exec with file caps), so it keeps running and would hold the ports.
// SIGTERM it (it removes its TUN state) before starting fresh. Only our user's mihomo with our run dir.
func (a *App) stopOrphans() {
	marker := []byte("\x00-d\x00" + a.runDir() + "\x00")
	ents, _ := os.ReadDir("/proc")
	for _, e := range ents {
		pid, err := strconv.Atoi(e.Name())
		if err != nil || pid == os.Getpid() || (a.cmd != nil && a.cmd.Process.Pid == pid) {
			continue
		}
		cmdline, err := os.ReadFile(filepath.Join("/proc", e.Name(), "cmdline"))
		if err != nil || !bytes.Contains(cmdline, marker) ||
			filepath.Base(string(bytes.SplitN(cmdline, []byte{0}, 2)[0])) != "mihomo" {
			continue
		}
		if st, err := os.Stat(filepath.Join("/proc", e.Name())); err != nil || st.Sys().(*syscall.Stat_t).Uid != uint32(os.Getuid()) {
			continue
		}
		log.Printf("stopping mihomo pid %d left from an earlier run (SIGTERM)", pid)
		syscall.Kill(pid, syscall.SIGTERM)
		for i := 0; i < 100 && syscall.Kill(pid, 0) == nil; i++ {
			time.Sleep(100 * time.Millisecond)
		}
	}
}

// ruleLeftovers: priorities of TUN ip rules in `ip rule show` output (tunRuleIndex .. +99; none on a stock Frame).
func ruleLeftovers(ipRuleOutput string) []string {
	var left []string
	for _, line := range strings.Split(ipRuleOutput, "\n") {
		prio, _, ok := strings.Cut(line, ":")
		if n, err := strconv.Atoi(prio); ok && err == nil && n >= tunRuleIndex && n < tunRuleIndex+100 {
			left = append(left, prio)
		}
	}
	return left
}

// tunLeftovers: TUN state in the kernel (call it with no TUN mihomo running).
func tunLeftovers() []string {
	var left []string
	for _, fam := range []string{"-4", "-6"} {
		out, _ := exec.Command("ip", fam, "rule", "show").Output()
		for _, p := range ruleLeftovers(string(out)) {
			left = append(left, "ip "+fam+" rule "+p)
		}
	}
	if _, err := os.Stat("/sys/class/net/" + tunDevice); err == nil {
		left = append(left, "device "+tunDevice)
	}
	return left
}

// repairLocked: stop everything, then remove TUN leftovers without root: a TUN-only run of the setcap mihomo
// replaces leftover rules when it starts and removes them all on SIGTERM. Leaves FrameClash stopped.
func (a *App) repairLocked() (string, error) {
	if err := a.stopLocked(15 * time.Second); err != nil {
		return "", err
	}
	a.state.Enabled = false
	a.saveState()
	a.stopOrphans()
	left := tunLeftovers()
	if len(left) == 0 {
		return "The network is clean (no TUN rules or device). FrameClash is stopped; press Start to use the proxy.", nil
	}
	if err := tunReady(a.home); err != nil {
		return "", fmt.Errorf("TUN leftovers (%s) but no TUN helper to remove them: run tun-setup.sh cleanup "+
			"(sudo) or reboot", strings.Join(left, ", "))
	}
	cfg, _ := yaml.Marshal(map[string]any{
		"tun": map[string]any{"enable": true, "device": tunDevice, "iproute2-table-index": tunTable,
			"iproute2-rule-index": tunRuleIndex, "auto-route": true, "auto-detect-interface": true, "stack": "system",
			"route-exclude-address": tunExcludeAddr, "exclude-interface": []any{"wlanap", "usb0"}},
	})
	path := filepath.Join(a.runDir(), "repair.yaml")
	if err := os.WriteFile(path, cfg, 0600); err != nil {
		return "", err
	}
	defer os.Remove(path)
	cmd := exec.Command(tunBinary(a.home), "-d", a.runDir(), "-f", path)
	if err := cmd.Start(); err != nil {
		return "", err
	}
	exited := make(chan struct{})
	go func() { cmd.Wait(); close(exited) }()
	for i := 0; i < 50; i++ { // until it has taken over the leftovers (device up), at most 5 s
		if _, err := os.Stat("/sys/class/net/" + tunDevice); err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	time.Sleep(time.Second)
	cmd.Process.Signal(syscall.SIGTERM)
	select {
	case <-exited:
	case <-time.After(15 * time.Second):
		return "", fmt.Errorf("the repair run of mihomo did not exit after SIGTERM (pid %d)", cmd.Process.Pid)
	}
	if left2 := tunLeftovers(); len(left2) > 0 {
		return "", fmt.Errorf("still present: %s - run tun-setup.sh cleanup (sudo) or reboot", strings.Join(left2, ", "))
	}
	log.Printf("repair: removed %s", strings.Join(left, ", "))
	return fmt.Sprintf("Network repaired (removed %d TUN leftovers). FrameClash is stopped; press Start to use "+
		"the proxy.", len(left)), nil
}

func (a *App) hRepair(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	defer a.mu.Unlock()
	msg, err := a.repairLocked()
	if err != nil {
		errOut(w, err)
		return
	}
	a.lastErr = ""
	jsonOut(w, 200, map[string]any{"ok": true, "message": msg})
}
