package main

// The `frameclash` command (0.2.1): a thin client of frameclashd's loopback API, for SSH (e.g. from a Mac) and
// scripts. It is the same binary as frameclashd: installed as the symlink ~/.local/bin/frameclash, or run as
// `frameclashd cli ...`. It never prints a subscription URL (the daemon only returns masked ones).

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

var cliUsage = `frameclash ` + version + ` - control FrameClash (talks to frameclashd on 127.0.0.1)

usage: frameclash COMMAND [ARGS]

  status                         is the proxy running, which profile, the proxy address
  start | stop                   start / stop the proxy (stop also turns autostart off)
  tun on|off                     TUN mode: all normal traffic (wlan0) goes through the proxy; the streaming
                                 Wi-Fi (wlanap), usb0 and the LAN stay direct. Needs tun-setup.sh install once
  repair                         stop the proxy and remove anything TUN left behind (no root); the TUN
                                 setup stays, the proxy stays off until you start it
  tun dns on|off                 TUN: point the system DNS (systemd-resolved) into the tun (default on), or
                                 leave it as the network set it (web/QUIC are still routed by domain)
  mode rule|global|direct        outbound mode (saved; applied without a restart)
  exit-ip                        check where proxied traffic leaves to the internet (asks ipinfo.io)
  import-url URL [--name NAME]   import a subscription (URL "-" = read it from stdin, keeps it out of
                                 your shell history: frameclash import-url - --name Home)
  import-file PATH [--name NAME] import a Clash/mihomo YAML file under your home folder
  profiles | list                saved profiles (* = active) and YAML files found
  use ID|NAME                    switch to a saved profile (a subscription is downloaded again)
  refresh                        download the active subscription again / re-read the active file
  remove ID|NAME                 take a profile off the list (files are never deleted)
  groups                         proxy groups and their selected node
  nodes [GROUP]                  nodes of a group (default: the first selector group), with last delay
  node list [GROUP] | node select [GROUP] NODE   (same as nodes / select)
  select [GROUP] NODE            choose the node of a group
  delay [GROUP]                  test every node of a group, then list them by delay
  version

The daemon address comes from FRAMECLASH_UI_ADDR (default 127.0.0.1:7899) and must be loopback.
Over SSH: ssh steamos@HOST ~/.local/bin/frameclash status
`

type cliClient struct {
	base string
	hc   *http.Client
}

func newCLI() (*cliClient, error) {
	addr := env("FRAMECLASH_UI_ADDR", "127.0.0.1:7899")
	host, _, err := net.SplitHostPort(addr)
	if err != nil || !(host == "127.0.0.1" || host == "localhost" || host == "::1") {
		return nil, fmt.Errorf("FRAMECLASH_UI_ADDR %q: only a loopback address is allowed", addr)
	}
	// no proxy env: this must always reach the local daemon directly
	return &cliClient{base: "http://" + addr, hc: &http.Client{Timeout: 90 * time.Second, Transport: &http.Transport{Proxy: nil}}}, nil
}

// call: GET when in == nil, else POST JSON (with the X-FrameClash header the daemon requires).
func (c *cliClient) call(path string, in any, out any) error {
	var req *http.Request
	var err error
	if in == nil {
		req, err = http.NewRequest("GET", c.base+path, nil)
	} else {
		b, _ := json.Marshal(in)
		req, err = http.NewRequest("POST", c.base+path, bytes.NewReader(b))
		if err == nil {
			req.Header.Set("Content-Type", "application/json")
			req.Header.Set("X-FrameClash", "1")
		}
	}
	if err != nil {
		return err
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return fmt.Errorf("frameclashd is not reachable at %s (is it running? systemctl --user status frameclash)", c.base)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<20))
	if resp.StatusCode != 200 {
		var e struct{ Error, Message string }
		if json.Unmarshal(body, &e) == nil && (e.Error != "" || e.Message != "") {
			return errors.New(strings.TrimSpace(e.Error + e.Message))
		}
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
	if out != nil {
		return json.Unmarshal(body, out)
	}
	return nil
}

type cliProfile struct {
	ID, Kind, Name, Updated, URL, Path string
	Active                             bool
}

type cliProfiles struct {
	Active   string
	Dir      string
	Profiles []cliProfile
	Files    []struct{ Name, Path, Modified string }
}

type cliProxy struct {
	Name, Type, Now string
	All             []string
	History         []struct{ Delay int }
}

func cliMain(args []string) int {
	if len(args) == 0 || args[0] == "-h" || args[0] == "--help" || args[0] == "help" {
		fmt.Print(cliUsage)
		if len(args) == 0 {
			return 2
		}
		return 0
	}
	if args[0] == "version" || args[0] == "--version" {
		fmt.Println("frameclash", version)
		return 0
	}
	c, err := newCLI()
	if err == nil {
		err = c.run(args[0], args[1:])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "frameclash:", err)
		var u usageError
		if errors.As(err, &u) {
			return 2
		}
		return 1
	}
	return 0
}

type usageError struct{ msg string }

func (u usageError) Error() string { return u.msg + " (frameclash --help)" }

// splitName pulls "--name NAME" / "--name=NAME" out of args.
func splitName(args []string) (rest []string, name string, err error) {
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--name" || a == "-n":
			if i+1 >= len(args) {
				return nil, "", usageError{"--name needs a value"}
			}
			name = args[i+1]
			i++
		case strings.HasPrefix(a, "--name="):
			name = strings.TrimPrefix(a, "--name=")
		default:
			rest = append(rest, a)
		}
	}
	return rest, name, nil
}

func (c *cliClient) run(cmd string, args []string) error {
	switch cmd {
	case "status":
		return c.status()
	case "start", "stop":
		if err := c.call("/api/"+cmd, struct{}{}, nil); err != nil {
			return err
		}
		if cmd == "start" {
			fmt.Println("Starting the proxy…")
			time.Sleep(1500 * time.Millisecond)
			return c.status()
		}
		fmt.Println("Proxy stopped.")
		return nil
	case "tun":
		if len(args) == 2 && args[0] == "dns" && (args[1] == "on" || args[1] == "off") {
			if err := c.call("/api/tun/dns", map[string]bool{"enable": args[1] == "on"}, nil); err != nil {
				return err
			}
			fmt.Printf("TUN DNS %s.\n", args[1])
			time.Sleep(3 * time.Second) // the tun comes back and resolved is set up again
			return c.status()
		}
		if len(args) != 1 || (args[0] != "on" && args[0] != "off") {
			return usageError{"tun needs on, off or dns on|off"}
		}
		if err := c.call("/api/tun", map[string]bool{"enable": args[0] == "on"}, nil); err != nil {
			return err
		}
		fmt.Printf("TUN %s.\n", args[0])
		return c.status()
	case "mode":
		if len(args) != 1 {
			return usageError{"mode needs rule, global or direct"}
		}
		if err := c.call("/api/mode", map[string]string{"mode": args[0]}, nil); err != nil {
			return err
		}
		fmt.Printf("Mode %s.\n", args[0])
		return nil
	case "exit-ip":
		var out struct{ IP, Country, City, Org string }
		if err := c.call("/api/exitip", nil, &out); err != nil {
			return err
		}
		fmt.Printf("%s  %s %s  %s\n", out.IP, out.Country, out.City, out.Org)
		return nil
	case "repair":
		var out struct{ Message string }
		if err := c.call("/api/repair", struct{}{}, &out); err != nil {
			return err
		}
		fmt.Println(out.Message)
		return nil
	case "import-url", "import":
		rest, name, err := splitName(args)
		if err != nil {
			return err
		}
		if len(rest) != 1 {
			return usageError{"import-url needs one URL (or - to read it from stdin)"}
		}
		u := rest[0]
		if u == "-" {
			if fi, _ := os.Stdin.Stat(); fi != nil && fi.Mode()&os.ModeCharDevice != 0 {
				fmt.Fprint(os.Stderr, "Paste the subscription URL, then press Enter: ")
			}
			line, _ := bufio.NewReader(io.LimitReader(os.Stdin, 64<<10)).ReadString('\n')
			u = line
		}
		u = strings.TrimSpace(u)
		if !strings.HasPrefix(u, "http://") && !strings.HasPrefix(u, "https://") || strings.ContainsAny(u, " \t") {
			return errors.New("that is not an http(s) URL")
		}
		fmt.Println("Downloading the subscription…")
		var out struct{ ID, Name string }
		if err := c.call("/api/subscription", map[string]string{"url": u, "name": name}, &out); err != nil {
			return fmt.Errorf("import failed: %v", err)
		}
		fmt.Printf("Imported %q (%s) and made it active.\n", out.Name, out.ID)
		return nil
	case "import-file":
		rest, name, err := splitName(args)
		if err != nil {
			return err
		}
		if len(rest) != 1 {
			return usageError{"import-file needs one PATH"}
		}
		p := rest[0]
		if !strings.HasPrefix(p, "~") && !filepath.IsAbs(p) {
			if abs, err := filepath.Abs(p); err == nil {
				p = abs
			}
		}
		var out struct{ ID, Name string }
		if err := c.call("/api/local/path", map[string]string{"path": p, "name": name}, &out); err != nil {
			return fmt.Errorf("import failed: %v", err)
		}
		fmt.Printf("Imported %q (%s) and made it active.\n", out.Name, out.ID)
		return nil
	case "profiles", "list", "ls":
		return c.profiles()
	case "use", "activate", "remove", "rm", "delete":
		if len(args) != 1 {
			return usageError{cmd + " needs a profile ID or NAME"}
		}
		p, err := c.findProfile(args[0])
		if err != nil {
			return err
		}
		if cmd == "use" || cmd == "activate" {
			if p.Kind == "url" {
				fmt.Println("Downloading the subscription…")
			}
			if err := c.call("/api/profiles/activate", map[string]string{"id": p.ID}, nil); err != nil {
				return err
			}
			fmt.Printf("Now using %q.\n", p.Name)
			return nil
		}
		if err := c.call("/api/profiles/delete", map[string]string{"id": p.ID}, nil); err != nil {
			return err
		}
		fmt.Printf("Removed %q from the list (no files deleted).\n", p.Name)
		return nil
	case "refresh", "update":
		// re-activating the active profile downloads the URL again / re-reads the file from its path
		var ps cliProfiles
		if err := c.call("/api/profiles", nil, &ps); err != nil {
			return err
		}
		for _, p := range ps.Profiles {
			if p.ID == ps.Active {
				if p.Kind == "url" {
					fmt.Println("Downloading the subscription again…")
				}
				if err := c.call("/api/profiles/activate", map[string]string{"id": p.ID}, nil); err != nil {
					return fmt.Errorf("refresh failed: %v", err)
				}
				fmt.Printf("Refreshed %q.\n", p.Name)
				return nil
			}
		}
		return errors.New("no active profile - import one first (frameclash import-url / import-file)")
	case "groups":
		groups, all, err := c.groups()
		if err != nil {
			return err
		}
		for _, g := range groups {
			fmt.Printf("%-12s %s → %s\n", all[g].Type, g, all[g].Now)
		}
		return nil
	case "node":
		if len(args) == 0 {
			return usageError{"node list [GROUP] | node select [GROUP] NODE"}
		}
		switch args[0] {
		case "list", "ls":
			return c.run("nodes", args[1:])
		case "select", "use":
			return c.run("select", args[1:])
		}
		return usageError{"node list [GROUP] | node select [GROUP] NODE"}
	case "nodes":
		if len(args) > 1 {
			return usageError{"nodes [GROUP]"}
		}
		return c.nodes(args, false)
	case "select":
		if len(args) < 1 || len(args) > 2 {
			return usageError{"select [GROUP] NODE"}
		}
		groups, all, err := c.groups()
		if err != nil {
			return err
		}
		group, node := "", args[len(args)-1]
		if len(args) == 2 {
			group = args[0]
		} else if group = firstSelector(groups, all); group == "" {
			return errors.New("no selector group in this profile")
		}
		g, ok := all[group]
		if !ok {
			return fmt.Errorf("no group %q (see: frameclash groups)", group)
		}
		if !contains(g.All, node) {
			return fmt.Errorf("group %q has no node %q (see: frameclash nodes %q)", group, node, group)
		}
		if err := c.call("/api/select", map[string]string{"group": group, "name": node}, nil); err != nil {
			return err
		}
		fmt.Printf("%s → %s\n", group, node)
		return nil
	case "delay", "test":
		if len(args) > 1 {
			return usageError{"delay [GROUP]"}
		}
		return c.nodes(args, true)
	}
	return usageError{fmt.Sprintf("unknown command %q", cmd)}
}

func (c *cliClient) status() error {
	var st struct {
		Running, Enabled, Tun bool
		SelfDNS               bool   `json:"self_dns"`
		TunDNS                string `json:"tun_dns"`
	}
	var raw map[string]any
	if err := c.call("/api/status", nil, &raw); err != nil {
		return err
	}
	b, _ := json.Marshal(raw)
	_ = json.Unmarshal(b, &st)
	str := func(k string) string { s, _ := raw[k].(string); return s }
	state := "stopped"
	if st.Running {
		state = "running"
	}
	fmt.Printf("Proxy:     %s (autostart %s)\n", state, onOff(st.Enabled))
	if st.Running {
		fmt.Printf("Address:   %s (HTTP + SOCKS5)\n", str("proxy"))
	}
	tun := onOff(st.Tun)
	if r := str("tun_ready"); r != "" {
		tun += " - " + r
	} else if st.Tun {
		dns := map[string]string{"on": "system DNS goes through the tun", "off": "system DNS left as is",
			"no-permission": "system DNS left as is: no permission (tun-setup.sh install)", "": "starting"}[st.TunDNS]
		if dns == "" {
			dns = st.TunDNS // an error
		}
		if !st.Running {
			dns = map[bool]string{true: "will point the system DNS into the tun", false: "system DNS left as is"}[st.SelfDNS]
		}
		tun += " (DNS: " + dns + ")"
	}
	fmt.Printf("TUN:       %s\n", tun)
	fmt.Printf("Mode:      %s\n", str("mode"))
	var ps cliProfiles
	if c.call("/api/profiles", nil, &ps) == nil {
		name := "none - import one (frameclash import-url / import-file)"
		for _, p := range ps.Profiles {
			if p.Active {
				name = fmt.Sprintf("%s (%s)", p.Name, kindName(p.Kind))
			}
		}
		fmt.Printf("Profile:   %s\n", name)
	}
	if t := str("last_update"); t != "" {
		fmt.Printf("Updated:   %s\n", shortTime(t))
	}
	if e := str("error"); e != "" {
		fmt.Printf("Error:     %s\n", e)
	}
	fmt.Printf("Daemon:    frameclashd %s on %s\n", str("version"), strings.TrimPrefix(c.base, "http://"))
	return nil
}

func (c *cliClient) profiles() error {
	var ps cliProfiles
	if err := c.call("/api/profiles", nil, &ps); err != nil {
		return err
	}
	if len(ps.Profiles) == 0 {
		fmt.Println("No saved profiles yet. Import one: frameclash import-url URL  |  frameclash import-file PATH")
	}
	for _, p := range ps.Profiles {
		mark := " "
		if p.Active {
			mark = "*"
		}
		where := p.URL // already masked by the daemon (scheme://host/…)
		if p.Kind != "url" {
			where = p.Path
		}
		fmt.Printf("%s %s  %-24s %-12s %-16s %s\n", mark, p.ID, p.Name, kindName(p.Kind), shortTime(p.Updated), where)
	}
	if len(ps.Files) > 0 {
		fmt.Printf("\nYAML files you can import (frameclash import-file PATH):\n")
		for _, f := range ps.Files {
			fmt.Printf("  %s\n", f.Path)
		}
	}
	return nil
}

func (c *cliClient) findProfile(key string) (cliProfile, error) {
	var ps cliProfiles
	if err := c.call("/api/profiles", nil, &ps); err != nil {
		return cliProfile{}, err
	}
	var hits []cliProfile
	for _, p := range ps.Profiles {
		if p.ID == key {
			return p, nil
		}
		if strings.EqualFold(p.Name, key) || (len(key) >= 4 && strings.HasPrefix(p.ID, key)) {
			hits = append(hits, p)
		}
	}
	switch len(hits) {
	case 0:
		return cliProfile{}, fmt.Errorf("no profile %q (see: frameclash profiles)", key)
	case 1:
		return hits[0], nil
	}
	ids := []string{}
	for _, h := range hits {
		ids = append(ids, h.ID)
	}
	return cliProfile{}, fmt.Errorf("%q matches %d profiles (%s): use the ID", key, len(hits), strings.Join(ids, ", "))
}

// groups: the proxy groups in config order (GLOBAL's list), GLOBAL itself last.
func (c *cliClient) groups() ([]string, map[string]cliProxy, error) {
	var out struct{ Proxies map[string]cliProxy }
	if err := c.call("/api/proxies", nil, &out); err != nil {
		if strings.Contains(err.Error(), "connect") || strings.Contains(err.Error(), "not running") {
			return nil, nil, errors.New("the proxy is not running (frameclash start)")
		}
		return nil, nil, err
	}
	isGroup := func(t string) bool {
		switch t {
		case "Selector", "URLTest", "Fallback", "LoadBalance", "Relay":
			return true
		}
		return false
	}
	var groups []string
	seen := map[string]bool{}
	if g, ok := out.Proxies["GLOBAL"]; ok {
		for _, n := range g.All {
			if p, ok := out.Proxies[n]; ok && isGroup(p.Type) && !seen[n] {
				groups = append(groups, n)
				seen[n] = true
			}
		}
	}
	var extra []string
	for n, p := range out.Proxies {
		if isGroup(p.Type) && !seen[n] && n != "GLOBAL" {
			extra = append(extra, n)
		}
	}
	sort.Strings(extra)
	groups = append(groups, extra...)
	if _, ok := out.Proxies["GLOBAL"]; ok {
		groups = append(groups, "GLOBAL")
	}
	return groups, out.Proxies, nil
}

func firstSelector(groups []string, all map[string]cliProxy) string {
	for _, g := range groups {
		if all[g].Type == "Selector" && g != "GLOBAL" {
			return g
		}
	}
	if len(groups) > 0 {
		return groups[0]
	}
	return ""
}

func (c *cliClient) nodes(args []string, test bool) error {
	groups, all, err := c.groups()
	if err != nil {
		return err
	}
	group := firstSelector(groups, all)
	if len(args) == 1 {
		group = args[0]
	}
	g, ok := all[group]
	if !ok || group == "" {
		return fmt.Errorf("no group %q (see: frameclash groups)", group)
	}
	if test {
		fmt.Printf("Testing %d nodes of %s…\n", len(g.All), group)
		if err := c.call("/api/groupdelay", map[string]string{"group": group}, nil); err != nil {
			fmt.Fprintln(os.Stderr, "frameclash: delay test:", err)
		}
		if _, all, err = c.groups(); err != nil {
			return err
		}
		g = all[group]
	}
	type row struct {
		name  string
		delay int // -1 untested, 0 timeout
	}
	var rows []row
	for _, n := range g.All {
		d := -1
		if h := all[n].History; len(h) > 0 {
			d = h[len(h)-1].Delay
		}
		rows = append(rows, row{n, d})
	}
	if test {
		rank := func(d int) int {
			if d <= 0 {
				return 1 << 30
			}
			return d
		}
		sort.SliceStable(rows, func(i, j int) bool { return rank(rows[i].delay) < rank(rows[j].delay) })
	}
	fmt.Printf("%s (%s) → %s\n", group, g.Type, g.Now)
	for _, r := range rows {
		mark := " "
		if r.name == g.Now {
			mark = "*"
		}
		d := ""
		switch {
		case r.delay > 0:
			d = fmt.Sprintf("%d ms", r.delay)
		case r.delay == 0:
			d = "timeout"
		}
		fmt.Printf("%s %8s  %s\n", mark, d, r.name)
	}
	return nil
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

func onOff(b bool) string {
	if b {
		return "on"
	}
	return "off"
}

func kindName(k string) string {
	if k == "url" {
		return "subscription"
	}
	return "file"
}

func shortTime(t string) string {
	if tt, err := time.Parse(time.RFC3339, t); err == nil {
		return tt.Local().Format("2006-01-02 15:04")
	}
	return t
}
