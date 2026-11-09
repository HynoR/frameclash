package main

import (
	"bytes"
	"encoding/json/v2"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

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
	if dns["enable"] != true || dns["listen"] != nil || dns["fake-ip-range"] != tunFakeIPRange { // :53 needs privileges: dropped
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

func TestOpenLogAppendAndClear(t *testing.T) {
	path := filepath.Join(t.TempDir(), "mihomo.log")
	f, err := openLog(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.WriteString("before\n"); err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(path, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := f.WriteString("after\n"); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(path)
	if err != nil || string(content) != "after\n" || bytes.Contains(content, []byte{0}) {
		t.Fatalf("append after truncate = %q, %v", content, err)
	}
	f, err = openLog(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.Write(bytes.Repeat([]byte("x"), logRotateBytes+1)); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path+".prev", []byte("stale"), 0600); err != nil {
		t.Fatal(err)
	}
	f, err = openLog(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.WriteString("new start\n"); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	prev, err := os.Stat(path + ".prev")
	if err != nil || prev.Size() <= logRotateBytes {
		t.Fatalf("rotated previous log = %+v, %v", prev, err)
	}
	content, err = os.ReadFile(path)
	if err != nil || string(content) != "new start\n" {
		t.Fatalf("new log = %q, %v", content, err)
	}
}

func TestTailFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "mihomo.log")
	content := []byte("first\nsecond\nthird\n")
	if err := os.WriteFile(path, content, 0600); err != nil {
		t.Fatal(err)
	}
	tail, err := tailFile(path, int64(len("third\n")+3))
	if err != nil || string(tail) != "third\n" {
		t.Fatalf("tail from a partial line = %q, %v", tail, err)
	}
	tail, err = tailFile(path, int64(len(content)+10))
	if err != nil || !bytes.Equal(tail, content) {
		t.Fatalf("short file = %q, %v", tail, err)
	}
}

func TestTunSuggest(t *testing.T) {
	tests := []struct {
		name     string
		helper   string
		outdated bool
		selfDNS  bool
		allowed  *bool
		left     []string
		want     string
	}{
		{name: "missing helper", helper: "missing", selfDNS: true, want: "install"},
		{name: "outdated helper", outdated: true, selfDNS: true, want: "install"},
		{name: "DNS denied", selfDNS: true, allowed: new(false), want: "install"},
		{name: "repair leftovers", selfDNS: true, allowed: new(true), left: []string{"rule"}, want: "repair"},
		{name: "cleanup without helper", helper: "missing", left: []string{"rule"}, want: "cleanup"},
		{name: "DNS not used", allowed: new(false), want: ""},
		{name: "unknown DNS permission", selfDNS: true, want: ""},
		{name: "ready", selfDNS: true, allowed: new(true), want: ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := tunSuggest(tt.helper, tt.outdated, tt.selfDNS, tt.allowed, tt.left); got != tt.want {
				t.Errorf("tunSuggest = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestTunSetupScript(t *testing.T) {
	home := "/home/steamos"
	if got := tunSetupScript(home, home+"/.local/lib/frameclash/mihomo"); got != "~/.local/lib/frameclash/tun-setup.sh" {
		t.Errorf("script = %q", got)
	}
	if got := tunSetupScript(home, "/opt/frameclash/mihomo"); got != "/opt/frameclash/tun-setup.sh" {
		t.Errorf("external script = %q", got)
	}
}

const testConnsJSON = `{
  "downloadTotal": 300, "uploadTotal": 100, "memory": 42,
  "connections": [
    {"id":"a","upload":1,"download":10,"start":"2026-10-27T10:00:00Z","chains":["node-a","select"],
     "rule":"DOMAIN-SUFFIX","rulePayload":"example.com","metadata":{"network":"tcp","type":"Tun","host":"",
     "sniffHost":"sniff.example","destinationIP":"192.0.2.1","destinationPort":"443","process":"steam"}},
    {"id":"b","upload":2,"download":20,"start":"2026-10-27T10:01:00Z","chains":["DIRECT","DIRECT"],
     "rule":"MATCH","rulePayload":"","metadata":{"network":"udp","type":"HTTP","host":"","sniffHost":"",
     "destinationIP":"192.0.2.2","destinationPort":"53","process":""}},
    {"id":"c","upload":3,"download":30,"start":"2026-10-27T10:02:00Z","chains":["node-c","select"],
     "rule":"MATCH","rulePayload":"","metadata":{"network":"tcp","type":"Socks","host":"host.example",
     "sniffHost":"ignored.example","destinationIP":"192.0.2.3","destinationPort":"8443","process":"steam"}}
  ]
}`

func TestGroupConns(t *testing.T) {
	var in connsResp
	if err := json.Unmarshal([]byte(testConnsJSON), &in); err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 10, 27, 10, 3, 0, 0, time.UTC)
	groups := groupConns(in.Connections, "process", now)
	if len(groups) != 2 || groups[0].Key != "steam" || groups[0].Count != 2 || groups[0].OldestS != 180 ||
		groups[0].Upload != 4 || groups[1].Key != "?" {
		t.Fatalf("process groups = %+v", groups)
	}
	hosts := groupConns(in.Connections, "host", now)
	seen := map[string]bool{}
	for _, group := range hosts {
		seen[group.Key] = true
	}
	if len(hosts) != 3 || !seen["sniff.example"] || !seen["192.0.2.2"] || !seen["host.example"] || seen["ignored.example"] {
		t.Fatalf("host fallback groups = %+v", hosts)
	}
	if got := connKey(in.Connections[0], "rule"); got != "DOMAIN-SUFFIX,example.com" {
		t.Errorf("rule key = %q", got)
	}
	entries := listConns(in.Connections, "process", "steam", now)
	if len(entries) != 2 || entries[0].ID != "c" || entries[1].ID != "a" || entries[0].AgeS != 60 ||
		entries[0].Chain != "node-c" || entries[0].Group != "select" || entries[0].Port != 8443 {
		t.Fatalf("connection details = %+v", entries)
	}
	for raw, want := range map[string]int{"2": 2, "500": 200} {
		r := httptest.NewRequest(http.MethodGet, "/api/connections?limit="+raw, nil)
		got, err := connLimit(r)
		if err != nil || got != want {
			t.Errorf("limit %s = %d, %v; want %d", raw, got, err, want)
		}
	}
}

func TestCloseConnsByGroup(t *testing.T) {
	deleted := []string{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer s" {
			t.Errorf("authorization header = %q", r.Header.Get("Authorization"))
		}
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/connections":
			_, _ = w.Write([]byte(testConnsJSON))
		case r.Method == http.MethodDelete:
			deleted = append(deleted, r.URL.Path)
			w.WriteHeader(http.StatusNoContent)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	a := &App{ctlPort: server.Listener.Addr().(*net.TCPAddr).Port, secret: "s"}
	req := httptest.NewRequest(http.MethodPost, "/api/connections/close",
		strings.NewReader(`{"by":"process","key":"steam"}`))
	response := httptest.NewRecorder()
	a.hCloseConns(response, req)
	var out struct {
		OK     bool `json:"ok"`
		Closed int  `json:"closed"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &out); err != nil {
		t.Fatal(err)
	}
	if response.Code != http.StatusOK || !out.OK || out.Closed != 2 {
		t.Fatalf("close response = %d %s", response.Code, response.Body.String())
	}
	if want := []string{"/connections/a", "/connections/c"}; !slices.Equal(deleted, want) {
		t.Errorf("DELETE paths = %v, want %v", deleted, want)
	}
}

func TestParseCliConnOptions(t *testing.T) {
	opts, err := parseCliConnOptions(nil, false)
	if err != nil || opts.by != "process" || opts.limit != 50 {
		t.Fatalf("defaults = %+v, %v", opts, err)
	}
	opts, err = parseCliConnOptions([]string{"--by", "host", "--key=example.com", "-n", "20"}, false)
	if err != nil || opts.by != "host" || opts.key != "example.com" || opts.limit != 20 {
		t.Fatalf("detail options = %+v, %v", opts, err)
	}
	opts, err = parseCliConnOptions([]string{"--by", "process", "--key", "steam"}, true)
	if err != nil || opts.key != "steam" || opts.target != "" {
		t.Fatalf("close group options = %+v, %v", opts, err)
	}
	opts, err = parseCliConnOptions([]string{"all"}, true)
	if err != nil || opts.target != "all" {
		t.Fatalf("close all options = %+v, %v", opts, err)
	}
	if _, err := parseCliConnOptions([]string{"--by", "none", "--key", "x"}, false); err == nil {
		t.Error("accepted --by none with --key")
	}
	if _, err := parseCliConnOptions([]string{"--by", "invalid"}, false); err == nil {
		t.Error("accepted an invalid group")
	}
}

func TestRenderLogLevelSilent(t *testing.T) {
	a := &App{cfgDir: t.TempDir(), mixedPort: 7890, ctlPort: 9090, secret: "s"}
	out, err := a.render([]byte("proxies: []\nlog-level: info\n"))
	if err != nil || !strings.Contains(string(out), "log-level: silent") {
		t.Fatalf("profile log-level not silenced: %v\n%s", err, out)
	}
}
