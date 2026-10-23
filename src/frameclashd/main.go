// frameclashd: tiny controller for mihomo on the Steam Frame (MVP).
// - keeps the active profile (subscription URL or local YAML) under ~/.config/frameclash
// - renders runtime.yaml (forces mixed-port on 127.0.0.1, controller on 127.0.0.1, tun disabled)
// - supervises mihomo; stops it ONLY with SIGTERM (never SIGKILL)
// - serves a minimal web UI on 127.0.0.1:7899
package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"gopkg.in/yaml.v3"
)

var version = "0.5.0"

//go:embed ui/index.html
var uiFS embed.FS

type State struct {
	Source        string `json:"source"` // "", "url", "file"
	SubURL        string `json:"sub_url,omitempty"`
	SubUA         string `json:"sub_ua,omitempty"`
	Enabled       bool   `json:"enabled"`
	LastUpdate    string `json:"last_update,omitempty"`
	SkipSteamRule bool   `json:"skip_steam_rules"`
	// 0.2.1: saved profiles (see profiles.go) and which one is active
	Profiles  []Profile `json:"profiles,omitempty"`
	ActiveID  string    `json:"active_id,omitempty"`
	TUN       bool      `json:"tun"`            // 0.3.0, see tun.go
	NoSelfDNS bool      `json:"no_self_dns"`    // TUN: leave systemd-resolved alone (default: point it into the tun)
	Mode      string    `json:"mode,omitempty"` // 0.5.0: rule|global|direct, "" = rule (like Clash Verge, it overrides the profile)
}

type App struct {
	mu        sync.Mutex
	cfgDir    string // ~/.config/frameclash
	dataDir   string // ~/.local/share/frameclash
	home      string // path imports must stay under
	mihomo    string
	mixedPort int
	ctlPort   int
	secret    string
	state     State
	cmd       *exec.Cmd
	done      chan struct{}
	lastErr   string
	tunDNS    string // TUN running: "" (pending), "on", "off", tunDNSNoPermission or an error
	cmdID     int    // which start the running mihomo belongs to
	stopping  bool   // a stop was asked for: an exit now is not a crash
	logFile   string
}

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func main() {
	// `frameclash ...` (the ~/.local/bin/frameclash symlink) or `frameclashd cli ...`: the command-line client
	if filepath.Base(os.Args[0]) == "frameclash" {
		os.Exit(cliMain(os.Args[1:]))
	}
	if len(os.Args) > 1 && os.Args[1] == "cli" {
		os.Exit(cliMain(os.Args[2:]))
	}
	home, _ := os.UserHomeDir()
	xdgCfg := env("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	xdgData := env("XDG_DATA_HOME", filepath.Join(home, ".local/share"))
	listen := flag.String("listen", env("FRAMECLASH_UI_ADDR", "127.0.0.1:7899"), "web UI listen address")
	mihomo := flag.String("mihomo", env("FRAMECLASH_MIHOMO", filepath.Join(home, ".local/lib/frameclash/mihomo")), "mihomo binary")
	cfgDir := flag.String("config-dir", filepath.Join(xdgCfg, "frameclash"), "config dir")
	dataDir := flag.String("data-dir", filepath.Join(xdgData, "frameclash"), "data dir")
	mixed := flag.Int("mixed-port", atoi(env("FRAMECLASH_MIXED_PORT", "7890")), "mixed (http+socks) port on 127.0.0.1")
	ctl := flag.Int("controller-port", atoi(env("FRAMECLASH_CONTROLLER_PORT", "9090")), "mihomo controller port on 127.0.0.1")
	showVer := flag.Bool("version", false, "print version")
	flag.Parse()
	if *showVer {
		fmt.Println("frameclashd", version)
		return
	}
	host, _, err := net.SplitHostPort(*listen)
	if err != nil || !(host == "127.0.0.1" || host == "localhost" || host == "::1") {
		log.Fatalf("refusing to listen on %q: MVP only allows loopback", *listen)
	}
	a := &App{cfgDir: *cfgDir, dataDir: *dataDir, mihomo: *mihomo, mixedPort: *mixed, ctlPort: *ctl, home: home}
	if err := a.init(); err != nil {
		log.Fatal(err)
	}
	a.stopOrphans()
	if a.state.Enabled {
		if err := a.start(); err != nil {
			log.Printf("autostart failed: %v", err)
		}
	}
	srv := &http.Server{Addr: *listen, Handler: a.routes(), ReadHeaderTimeout: 10 * time.Second}
	go func() {
		log.Printf("frameclashd %s UI on http://%s", version, *listen)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatal(err)
		}
	}()
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
	<-sig
	a.mu.Lock()
	a.stopping = true // systemd (KillMode=control-group) SIGTERMs mihomo too: its exit is not a crash
	a.mu.Unlock()
	log.Printf("shutting down: stopping mihomo with SIGTERM")
	srv.Close()
	a.mu.Lock()
	a.stopLocked(25 * time.Second)
	a.mu.Unlock()
}

func atoi(s string) int { var n int; fmt.Sscanf(s, "%d", &n); return n }

func (a *App) p(parts ...string) string {
	return filepath.Join(append([]string{a.cfgDir}, parts...)...)
}
func (a *App) runDir() string { return filepath.Join(a.dataDir, "mihomo") }

func (a *App) init() error {
	for _, d := range []string{a.p("profiles"), a.runDir()} {
		if err := os.MkdirAll(d, 0700); err != nil {
			return err
		}
	}
	a.logFile = filepath.Join(a.dataDir, "mihomo.log")
	sec, err := os.ReadFile(a.p("secret"))
	if err != nil || len(strings.TrimSpace(string(sec))) < 16 {
		b := make([]byte, 16)
		rand.Read(b)
		sec = []byte(hex.EncodeToString(b))
		if err := os.WriteFile(a.p("secret"), sec, 0600); err != nil {
			return err
		}
	}
	a.secret = strings.TrimSpace(string(sec))
	a.state = State{SkipSteamRule: true}
	if b, err := os.ReadFile(a.p("state.json")); err == nil {
		json.Unmarshal(b, &a.state)
	}
	a.migrateProfiles()
	return nil
}

func (a *App) saveState() error {
	b, _ := json.MarshalIndent(a.state, "", "  ")
	return writeAtomic(a.p("state.json"), b, 0600)
}

// writeAtomic: tmp file, fsync, rename - a power loss leaves the old or the new file, never an empty one.
func writeAtomic(path string, b []byte, mode os.FileMode) error {
	tmp := path + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	_, err = f.Write(b)
	if err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

// profilePath returns the YAML the active source points to.
// profileBytes: the active profile's YAML. A file the user named by path is read from there (same checks as at
// import); a subscription / pasted YAML from its working copy.
func (a *App) profileBytes() ([]byte, error) {
	if i := a.findProfile(a.state.ActiveID); i >= 0 && a.state.Profiles[i].Path != "" {
		_, b, err := readUserFile(a.home, a.state.Profiles[i].Path)
		return b, err
	}
	return os.ReadFile(a.profilePath())
}

func (a *App) profilePath() string {
	switch a.state.Source {
	case "url":
		return a.p("profiles", "subscription.yaml")
	case "file":
		return a.p("profiles", "local.yaml")
	}
	return ""
}

var skipSteamRules = []any{
	"DOMAIN-SUFFIX,cm.steampowered.com,DIRECT",
	"DOMAIN-SUFFIX,steamserver.net,DIRECT",
}

// render converts a user/provider config into the runtime config with FrameClash's forced settings.
func (a *App) render(src []byte) ([]byte, error) {
	var cfg map[string]any
	if err := yaml.Unmarshal(src, &cfg); err != nil {
		return nil, fmt.Errorf("not valid YAML: %w", err)
	}
	if cfg == nil {
		return nil, errors.New("empty config")
	}
	// ~/.config/frameclash/override.yaml: the user's own changes on top of every profile (like Clash Verge's
	// override), e.g. dns.listen. FrameClash's forced settings below still win.
	if b, err := os.ReadFile(a.p("override.yaml")); err == nil {
		var ov map[string]any
		if err := yaml.Unmarshal(b, &ov); err != nil {
			return nil, fmt.Errorf("override.yaml: %w", err)
		}
		merge(cfg, ov)
	}
	_, hasProxies := cfg["proxies"]
	_, hasProviders := cfg["proxy-providers"]
	if !hasProxies && !hasProviders {
		return nil, errors.New("no 'proxies' or 'proxy-providers' in config (is this a Clash/mihomo YAML? some providers need '&flag=clash' or a clash-format link)")
	}
	for _, k := range []string{"port", "socks-port", "redir-port", "tproxy-port", "external-ui", "external-ui-url", "external-ui-name", "external-controller-tls", "external-controller-unix", "external-controller-pipe", "external-controller-cors", "authentication", "listeners", "iptables", "ebpf", "routing-mark", "interface-name"} {
		delete(cfg, k)
	}
	cfg["mixed-port"] = a.mixedPort
	cfg["allow-lan"] = false
	cfg["bind-address"] = "127.0.0.1"
	cfg["external-controller"] = fmt.Sprintf("127.0.0.1:%d", a.ctlPort)
	cfg["secret"] = a.secret
	cfg["mode"] = a.mode()
	if a.state.TUN {
		applyTun(cfg)
	} else {
		cfg["tun"] = map[string]any{"enable": false}
	}
	cfg["geo-auto-update"] = false
	cfg["unified-delay"] = true
	prof, _ := cfg["profile"].(map[string]any)
	if prof == nil {
		prof = map[string]any{}
	}
	prof["store-selected"] = true
	cfg["profile"] = prof
	if dns, ok := cfg["dns"].(map[string]any); ok {
		// Profiles point proxy-server-nameserver etc. at their own listener (e.g. udp://127.0.0.1:7874), so keep it,
		// on loopback. A port below 1024 can't be bound without privileges: dropped.
		l, _ := dns["listen"].(string)
		delete(dns, "listen")
		if _, port, err := net.SplitHostPort(l); err == nil && atoi(port) >= 1024 {
			dns["listen"] = net.JoinHostPort("127.0.0.1", port)
		}
	}
	if a.state.SkipSteamRule {
		if rules, ok := cfg["rules"].([]any); ok {
			cfg["rules"] = append(append([]any{}, skipSteamRules...), rules...)
		}
	}
	return yaml.Marshal(cfg)
}

// merge: maps merge recursively, anything else (lists too) is replaced.
func merge(dst, src map[string]any) {
	for k, v := range src {
		if sm, ok := v.(map[string]any); ok {
			if dm, ok := dst[k].(map[string]any); ok {
				merge(dm, sm)
				continue
			}
		}
		dst[k] = v
	}
}

// check runs `mihomo -t` against candidate bytes.
func (a *App) check(rendered []byte) error {
	tmp := filepath.Join(a.runDir(), "check.yaml")
	if err := os.WriteFile(tmp, rendered, 0600); err != nil {
		return err
	}
	defer os.Remove(tmp)
	out, err := exec.Command(a.mihomo, "-d", a.runDir(), "-f", tmp, "-t").CombinedOutput()
	if err != nil {
		s := string(out)
		if len(s) > 1500 {
			s = s[len(s)-1500:]
		}
		return fmt.Errorf("mihomo -t failed: %v\n%s", err, s)
	}
	return nil
}

func (a *App) writeRuntime() error {
	src, err := a.profileBytes()
	if err != nil {
		return err
	}
	r, err := a.render(src)
	if err != nil {
		return err
	}
	return writeAtomic(filepath.Join(a.runDir(), "runtime.yaml"), r, 0600)
}

func (a *App) running() bool { return a.cmd != nil && a.done != nil && !isClosed(a.done) }

func isClosed(c chan struct{}) bool {
	select {
	case <-c:
		return true
	default:
		return false
	}
}

func (a *App) start() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.startLocked()
}

func (a *App) startLocked() error {
	if a.running() {
		return nil
	}
	if a.profilePath() == "" {
		return errors.New("no profile imported yet")
	}
	if err := a.writeRuntime(); err != nil {
		a.lastErr = err.Error()
		return err
	}
	lf, err := os.OpenFile(a.logFile, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0600)
	if err != nil {
		return err
	}
	bin := a.mihomo
	if a.state.TUN {
		if err := tunReady(a.home); err != nil {
			lf.Close()
			a.lastErr = err.Error()
			return err
		}
		// ponytail: the kernel clears Pdeathsig when exec gains file caps, so a crashed frameclashd leaves this
		// mihomo running (still routing fine); a later start then fails on the busy ports until it is stopped.
		bin = tunBinary(a.home)
	}
	cmd := exec.Command(bin, "-d", a.runDir(), "-f", filepath.Join(a.runDir(), "runtime.yaml"))
	cmd.Stdout, cmd.Stderr = lf, lf
	cmd.Env = append(os.Environ(), "SAFE_PATHS="+a.runDir())
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGTERM}
	if err := cmd.Start(); err != nil {
		lf.Close()
		a.lastErr = err.Error()
		return err
	}
	done := make(chan struct{})
	a.cmd, a.done = cmd, done
	a.lastErr = ""
	a.cmdID++
	a.stopping = false
	id := a.cmdID
	go func() {
		err := cmd.Wait()
		lf.Close()
		close(done)
		a.mu.Lock()
		if a.cmdID == id && !a.stopping {
			why := "exited"
			if err != nil {
				why = err.Error()
			}
			a.lastErr = "mihomo stopped unexpectedly (" + why + ", see " + a.logFile + ")"
		}
		a.mu.Unlock()
		log.Printf("mihomo pid %d exited: %v", cmd.Process.Pid, err)
	}()
	a.tunDNS = ""
	if a.state.TUN {
		selfDNS := !a.state.NoSelfDNS
		go func() {
			st := "off"
			if !waitTun(done) {
				return
			}
			if selfDNS {
				st = setTunDNS(true)
			}
			a.mu.Lock()
			if a.cmd == cmd {
				a.tunDNS = st
			}
			a.mu.Unlock()
		}()
	}
	log.Printf("mihomo started pid %d", cmd.Process.Pid)
	return nil
}

// stopLocked: SIGTERM and wait. Never SIGKILL (mihomo cleans up its own routes/rules on SIGTERM).
func (a *App) stopLocked(wait time.Duration) error {
	if !a.running() {
		a.cmd = nil
		return nil
	}
	a.stopping = true
	log.Printf("sending SIGTERM to mihomo pid %d", a.cmd.Process.Pid)
	a.cmd.Process.Signal(syscall.SIGTERM)
	select {
	case <-a.done:
		a.cmd = nil
		return nil
	case <-time.After(wait):
		return fmt.Errorf("mihomo did not exit within %s after SIGTERM (not force-killing)", wait)
	}
}

func (a *App) restartIfRunning() error {
	if a.running() {
		if err := a.stopLocked(15 * time.Second); err != nil {
			return err
		}
		return a.startLocked()
	}
	return nil
}

func (a *App) userAgent() string {
	if a.state.SubUA != "" {
		return a.state.SubUA
	}
	return "mihomo/1.19.30 clash.meta/1.19.30 clash-verge/2.5.0 FrameClash/" + version
}

func download(u, ua string) ([]byte, error) {
	pu, err := url.Parse(u)
	if err != nil || (pu.Scheme != "http" && pu.Scheme != "https") {
		return nil, errors.New("subscription URL must be http(s)")
	}
	req, _ := http.NewRequest("GET", u, nil)
	req.Header.Set("User-Agent", ua)
	// Direct download (not through mihomo): works even before the first start.
	cl := &http.Client{Timeout: 30 * time.Second, Transport: &http.Transport{Proxy: nil}}
	resp, err := cl.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, maxImportBytes+1))
	if err == nil && len(b) > maxImportBytes {
		return nil, errors.New("subscription too large (max 32 MB)")
	}
	return b, err
}

// importBytes validates a profile's YAML, stores its working copy, makes it the active profile and restarts
// mihomo if it runs. Working copies: profiles/subscription.yaml (URL), profiles/local.yaml (pasted / hand-edited);
// a file the user named by path gets none, it is read from there (so importing it never clobbers local.yaml).
func (a *App) importBytes(p Profile, data []byte) (Profile, error) {
	r, err := a.render(data)
	if err != nil {
		return p, err
	}
	if err := a.check(r); err != nil {
		return p, err
	}
	a.state.Source = p.Kind
	switch {
	case p.Kind == "url":
		err = writeAtomic(a.p("profiles", "subscription.yaml"), data, 0600)
		a.state.SubURL = p.URL
	case p.Path == "":
		err = writeAtomic(a.p("profiles", "local.yaml"), data, 0600)
	}
	if err != nil {
		return p, err
	}
	a.state.LastUpdate = time.Now().Format(time.RFC3339)
	p = a.upsertProfile(p) // active + saved
	return p, a.restartIfRunning()
}

// ---------------- HTTP ----------------

func (a *App) routes() http.Handler {
	m := http.NewServeMux()
	m.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		b, _ := uiFS.ReadFile("ui/index.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(b)
	})
	m.HandleFunc("GET /api/status", a.hStatus)
	m.HandleFunc("POST /api/start", a.hStart)
	m.HandleFunc("POST /api/stop", a.hStop)
	m.HandleFunc("POST /api/subscription", a.hSub)
	m.HandleFunc("POST /api/subscription/refresh", a.hSubRefresh)
	m.HandleFunc("POST /api/local", a.hLocal)
	m.HandleFunc("POST /api/local/reload", a.hLocalReload)
	m.HandleFunc("POST /api/local/path", a.hLocalPath)
	m.HandleFunc("GET /api/profiles", a.hProfiles)
	m.HandleFunc("POST /api/profiles/activate", a.hProfileActivate)
	m.HandleFunc("POST /api/profiles/delete", a.hProfileDelete)
	m.HandleFunc("POST /api/tun", a.hTun)
	m.HandleFunc("POST /api/tun/dns", a.hTunDNS)
	m.HandleFunc("POST /api/repair", a.hRepair)
	m.HandleFunc("POST /api/mode", a.hMode)
	m.HandleFunc("GET /api/exitip", a.hExitIP)
	m.HandleFunc("GET /api/proxies", a.hProxies)
	m.HandleFunc("POST /api/select", a.hSelect)
	m.HandleFunc("POST /api/delay", a.hDelay)
	m.HandleFunc("POST /api/groupdelay", a.hGroupDelay)
	m.HandleFunc("GET /api/conn", a.hConn)
	m.HandleFunc("GET /api/log", a.hLog)
	return guard(m)
}

// guard: loopback Host only (anti DNS-rebinding) and a custom header on POST (anti CSRF).
func guard(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host := r.Host
		if hh, _, err := net.SplitHostPort(host); err == nil {
			host = hh
		}
		if host != "127.0.0.1" && host != "localhost" && host != "[::1]" && host != "::1" {
			http.Error(w, "forbidden host", 403)
			return
		}
		if r.Method != "GET" && r.Header.Get("X-FrameClash") != "1" {
			http.Error(w, "missing X-FrameClash header", 403)
			return
		}
		h.ServeHTTP(w, r)
	})
}

func jsonOut(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func errOut(w http.ResponseWriter, err error) {
	jsonOut(w, 400, map[string]string{"error": err.Error()})
}

func maskURL(u string) string {
	pu, err := url.Parse(u)
	if err != nil || u == "" {
		return ""
	}
	return pu.Scheme + "://" + pu.Host + "/…"
}

func (a *App) hStatus(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	defer a.mu.Unlock()
	tunErr := ""
	if err := tunReady(a.home); err != nil {
		tunErr = err.Error()
	}
	pid := 0
	if a.running() {
		pid = a.cmd.Process.Pid
	}
	jsonOut(w, 200, map[string]any{
		"version": version, "running": a.running(), "pid": pid, "enabled": a.state.Enabled,
		"source": a.state.Source, "sub_url": maskURL(a.state.SubURL), "last_update": a.state.LastUpdate,
		"error": a.lastErr, "mixed_port": a.mixedPort, "proxy": fmt.Sprintf("http://127.0.0.1:%d", a.mixedPort),
		"local_path": a.p("profiles", "local.yaml"), "tun": a.state.TUN, "tun_ready": tunErr,
		"self_dns": !a.state.NoSelfDNS, "tun_dns": a.tunDNS, "mode": a.mode(),
	})
}

func (a *App) hStart(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if err := a.startLocked(); err != nil {
		errOut(w, err)
		return
	}
	a.state.Enabled = true
	a.saveState()
	jsonOut(w, 200, map[string]bool{"ok": true})
}

func (a *App) hStop(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if err := a.stopLocked(15 * time.Second); err != nil {
		errOut(w, err) // still running: autostart stays as it was
		return
	}
	a.state.Enabled = false
	a.saveState()
	jsonOut(w, 200, map[string]bool{"ok": true})
}

func readJSON(r *http.Request, v any) error {
	return json.NewDecoder(io.LimitReader(r.Body, 32<<20)).Decode(v)
}

func (a *App) hSub(w http.ResponseWriter, r *http.Request) {
	var in struct{ URL, UA, Name string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	in.URL = strings.TrimSpace(in.URL)
	a.mu.Lock()
	if in.UA != "" {
		a.state.SubUA = strings.TrimSpace(in.UA)
	}
	ua := a.userAgent()
	a.mu.Unlock()
	// Downloaded without holding the lock: /api/status keeps answering during a slow download
	data, err := download(in.URL, ua)
	if err != nil {
		errOut(w, fmt.Errorf("download failed: %w", err))
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	p, err := a.importBytes(Profile{Kind: "url", URL: in.URL, Name: in.Name}, data)
	if err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]any{"ok": true, "id": p.ID, "name": p.Name})
}

// hSubRefresh: refresh the active profile (download it again / re-read the file), like `frameclash refresh`.
// It used to re-download the last subscription URL even when a file was active, switching profiles silently.
func (a *App) hSubRefresh(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	id := a.state.ActiveID
	a.mu.Unlock()
	if _, err := a.activate(id); err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]bool{"ok": true})
}

func (a *App) hLocal(w http.ResponseWriter, r *http.Request) {
	var in struct{ YAML string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if _, err := a.importBytes(Profile{Kind: "file", Name: "Pasted YAML"}, []byte(in.YAML)); err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]bool{"ok": true})
}

// hLocalReload: user placed/edited ~/.config/frameclash/profiles/local.yaml by hand.
func (a *App) hLocalReload(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	defer a.mu.Unlock()
	data, err := os.ReadFile(a.p("profiles", "local.yaml"))
	if err != nil {
		errOut(w, err)
		return
	}
	if _, err := a.importBytes(Profile{Kind: "file", Name: "Local YAML"}, data); err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]bool{"ok": true})
}

func (a *App) ctl(method, path string, body io.Reader) (*http.Response, error) {
	req, _ := http.NewRequest(method, fmt.Sprintf("http://127.0.0.1:%d%s", a.ctlPort, path), body)
	req.Header.Set("Authorization", "Bearer "+a.secret)
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	cl := &http.Client{Timeout: 12 * time.Second, Transport: &http.Transport{Proxy: nil}}
	return cl.Do(req)
}

func (a *App) passthru(w http.ResponseWriter, resp *http.Response, err error) {
	if err != nil {
		errOut(w, fmt.Errorf("mihomo controller: %w", err))
		return
	}
	defer resp.Body.Close()
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(resp.StatusCode)
	io.Copy(w, resp.Body)
}

func (a *App) hProxies(w http.ResponseWriter, r *http.Request) {
	resp, err := a.ctl("GET", "/proxies", nil)
	a.passthru(w, resp, err)
}

func (a *App) hSelect(w http.ResponseWriter, r *http.Request) {
	var in struct{ Group, Name string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	b, _ := json.Marshal(map[string]string{"name": in.Name})
	resp, err := a.ctl("PUT", "/proxies/"+url.PathEscape(in.Group), strings.NewReader(string(b)))
	if err == nil && resp.StatusCode == 204 {
		resp.Body.Close()
		jsonOut(w, 200, map[string]bool{"ok": true})
		return
	}
	a.passthru(w, resp, err)
}

func (a *App) hDelay(w http.ResponseWriter, r *http.Request) {
	var in struct{ Name string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	resp, err := a.ctl("GET", "/proxies/"+url.PathEscape(in.Name)+"/delay?timeout=5000&url="+url.QueryEscape("https://www.gstatic.com/generate_204"), nil)
	a.passthru(w, resp, err)
}

// hGroupDelay: test every node of a group at once (mihomo /group/<name>/delay, which also updates each node's history).
func (a *App) hGroupDelay(w http.ResponseWriter, r *http.Request) {
	var in struct{ Group string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	resp, err := a.ctl("GET", "/group/"+url.PathEscape(in.Group)+"/delay?timeout=5000&url="+url.QueryEscape("https://www.gstatic.com/generate_204"), nil)
	a.passthru(w, resp, err)
}

// hConn: a small summary of mihomo /connections (totals and count), so the VR panel doesn't parse the full list.
func (a *App) hConn(w http.ResponseWriter, r *http.Request) {
	resp, err := a.ctl("GET", "/connections", nil)
	if err != nil {
		errOut(w, fmt.Errorf("mihomo controller: %w", err))
		return
	}
	defer resp.Body.Close()
	var in struct {
		DownloadTotal int64             `json:"downloadTotal"`
		UploadTotal   int64             `json:"uploadTotal"`
		Connections   []json.RawMessage `json:"connections"`
		Memory        int64             `json:"memory"`
	}
	if resp.StatusCode != 200 {
		errOut(w, fmt.Errorf("mihomo controller: HTTP %d", resp.StatusCode))
		return
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 64<<20)).Decode(&in); err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]any{"download_total": in.DownloadTotal, "upload_total": in.UploadTotal,
		"connections": len(in.Connections), "memory": in.Memory})
}

func (a *App) mode() string {
	if a.state.Mode == "" {
		return "rule"
	}
	return a.state.Mode
}

// hMode: outbound mode, saved and applied live (mihomo PATCH /configs, no restart).
func (a *App) hMode(w http.ResponseWriter, r *http.Request) {
	var in struct{ Mode string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	if in.Mode != "rule" && in.Mode != "global" && in.Mode != "direct" {
		errOut(w, fmt.Errorf("mode must be rule, global or direct"))
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	a.state.Mode = in.Mode
	a.saveState()
	if a.running() {
		resp, err := a.ctl("PATCH", "/configs", strings.NewReader(`{"mode":"`+in.Mode+`"}`))
		if err != nil {
			errOut(w, fmt.Errorf("mihomo controller: %w", err))
			return
		}
		resp.Body.Close()
		if resp.StatusCode != 204 {
			errOut(w, fmt.Errorf("mihomo controller: HTTP %d", resp.StatusCode))
			return
		}
	}
	jsonOut(w, 200, map[string]bool{"ok": true})
}

// hExitIP: on demand only (the panel's "Check" button): where traffic through the proxy leaves to the internet.
func (a *App) hExitIP(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	running, port := a.running(), a.mixedPort
	a.mu.Unlock()
	if !running {
		errOut(w, errors.New("the proxy is not running"))
		return
	}
	proxy, _ := url.Parse(fmt.Sprintf("http://127.0.0.1:%d", port))
	cl := &http.Client{Timeout: 10 * time.Second, Transport: &http.Transport{Proxy: http.ProxyURL(proxy)}}
	resp, err := cl.Get("https://ipinfo.io/json")
	if err != nil {
		errOut(w, err)
		return
	}
	defer resp.Body.Close()
	var in struct{ IP, Country, City, Org string }
	if resp.StatusCode != 200 {
		errOut(w, fmt.Errorf("ipinfo.io: HTTP %d", resp.StatusCode))
		return
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<16)).Decode(&in); err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]string{"ip": in.IP, "country": in.Country, "city": in.City, "org": in.Org})
}

func (a *App) hLog(w http.ResponseWriter, r *http.Request) {
	b, _ := os.ReadFile(a.logFile)
	if len(b) > 16000 {
		b = b[len(b)-16000:]
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Write(b)
}
