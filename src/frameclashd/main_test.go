package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"gopkg.in/yaml.v3"
)

func TestRenderTun(t *testing.T) {
	src := []byte("proxies: []\ntun: {enable: true, auto-redirect: true}\n" +
		"dns: {enable: false, listen: 0.0.0.0:53, enhanced-mode: fake-ip, fake-ip-filter: ['*.lan', '+.example']}\n")
	a := &App{mixedPort: 7890, ctlPort: 9090, secret: "s"}

	out, err := a.render(src) // TUN off: the profile's tun is overridden
	if err != nil || !strings.Contains(string(out), "tun:\n    enable: false") {
		t.Fatalf("tun off: %v\n%s", err, out)
	}

	a.state.TUN = true
	out, err = a.render(src)
	if err != nil {
		t.Fatal(err)
	}
	var cfg map[string]any
	yaml.Unmarshal(out, &cfg)
	tun := cfg["tun"].(map[string]any)
	dns := cfg["dns"].(map[string]any)
	if tun["enable"] != true || tun["auto-redirect"] != false || tun["exclude-interface"].([]any)[0] != "wlanap" {
		t.Errorf("tun = %v", tun)
	}
	if dns["enable"] != true || dns["listen"] != nil { // :53 needs privileges: dropped
		t.Errorf("dns = %v", dns)
	}
	// a high listen port is kept, on loopback (profiles resolve their nodes through it)
	out, _ = a.render([]byte("proxies: []\ndns: {enable: true, listen: 0.0.0.0:7874}\n"))
	if !strings.Contains(string(out), "listen: 127.0.0.1:7874") {
		t.Errorf("dns listen not kept on loopback:\n%s", out)
	}
	if f := dns["fake-ip-filter"].([]any); len(f) != len(tunFakeIPFilter)+1 || f[1] != "+.example" {
		t.Errorf("fake-ip-filter = %v", f) // '*.lan' kept once, the profile's own entries kept
	}
	if cfg["sniffer"] == nil {
		t.Error("no sniffer")
	}
}

func TestOverride(t *testing.T) {
	a := &App{cfgDir: t.TempDir(), mixedPort: 7890, ctlPort: 9090, secret: "s"}
	os.WriteFile(a.p("override.yaml"), []byte("dns: {listen: 0.0.0.0:1053}\nallow-lan: true\nlog-level: debug\n"), 0600)
	out, err := a.render([]byte("proxies: []\ndns: {enable: true, listen: 0.0.0.0:7874, ipv6: true}\n"))
	s := string(out)
	if err != nil || !strings.Contains(s, "listen: 127.0.0.1:1053") || !strings.Contains(s, "ipv6: true") ||
		!strings.Contains(s, "log-level: debug") || !strings.Contains(s, "allow-lan: false") {
		t.Errorf("override not applied as expected (%v):\n%s", err, s)
	}
}

// Importing a YAML file by path must not overwrite profiles/local.yaml (the pasted / hand-edited one), and refresh
// re-reads the active profile instead of re-downloading an old subscription URL.
func TestImportKeepsLocalYAML(t *testing.T) {
	home := t.TempDir()
	a := &App{cfgDir: filepath.Join(home, "cfg"), dataDir: filepath.Join(home, "data"), home: home,
		mihomo: "/bin/true", mixedPort: 7890, ctlPort: 9090, secret: "s"}
	if err := a.init(); err != nil {
		t.Fatal(err)
	}
	pasted := []byte("proxies: [{name: pasted, type: socks5, server: 1.1.1.1, port: 1}]\n")
	if _, err := a.importBytes(Profile{Kind: "file", Name: "Pasted YAML"}, pasted); err != nil {
		t.Fatal(err)
	}
	user := filepath.Join(home, "Downloads", "mine.yaml")
	os.MkdirAll(filepath.Dir(user), 0700)
	os.WriteFile(user, []byte("proxies: [{name: mine, type: socks5, server: 2.2.2.2, port: 1}]\n"), 0600)
	real, data, err := readUserFile(home, user)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := a.importBytes(Profile{Kind: "file", Path: real}, data); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(a.p("profiles", "local.yaml")); string(b) != string(pasted) {
		t.Fatalf("local.yaml overwritten: %s", b)
	}
	if b, _ := a.profileBytes(); !strings.Contains(string(b), "mine") {
		t.Fatalf("active profile reads %s", b)
	}
	if len(a.state.Profiles) != 2 {
		t.Fatalf("profiles = %+v", a.state.Profiles)
	}
	if _, err := a.activate(a.state.Profiles[0].ID); err != nil { // back to the pasted one
		t.Fatal(err)
	}
	if b, _ := a.profileBytes(); !strings.Contains(string(b), "pasted") {
		t.Fatalf("pasted profile lost: %s", b)
	}
}

func TestRuleLeftovers(t *testing.T) {
	out := "0:\tfrom all lookup local\n9000:\tfrom all iif wlanap goto 9010\n9003:\tnot from all iif lo lookup 2022\n" +
		"9010:\tfrom all nop\n32766:\tfrom all lookup main\n"
	if got := strings.Join(ruleLeftovers(out), ","); got != "9000,9003,9010" {
		t.Errorf("ruleLeftovers = %s", got)
	}
	if ruleLeftovers("0:\tfrom all lookup local\n32766:\tfrom all lookup main\n") != nil {
		t.Error("stock rules reported as leftovers")
	}
}

func TestRenderMode(t *testing.T) {
	a := &App{mixedPort: 7890, ctlPort: 9090, secret: "s"}
	src := []byte("mode: global\nproxies: []\n")
	if out, _ := a.render(src); !strings.Contains(string(out), "mode: rule") { // FrameClash's mode wins, default rule
		t.Errorf("default mode:\n%s", out)
	}
	a.state.Mode = "direct"
	if out, _ := a.render(src); !strings.Contains(string(out), "mode: direct") {
		t.Errorf("mode direct:\n%s", out)
	}
}
