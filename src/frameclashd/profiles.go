// Saved profiles (subscription URLs and local YAML files) for the VR panel's Import/Profiles view (0.2.1).
// Only one profile is active at a time and frameclashd still keeps a single working copy of it
// (profiles/subscription.yaml or profiles/local.yaml); the list just remembers where each one came from, so
// "Use"/"Refresh" re-downloads the URL or re-reads the file. Deleting an entry never deletes a user file.
package main

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type Profile struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Kind    string `json:"kind"`           // "url" or "file"
	URL     string `json:"url,omitempty"`  // kind url (kept in state.json, 0600; masked in every API answer)
	Path    string `json:"path,omitempty"` // kind file: the user's file; "" = YAML pasted into the web UI (profiles/local.yaml)
	Updated string `json:"updated,omitempty"`
}

const maxProfiles = 32
const maxImportBytes = 32 << 20

func newID() string {
	b := make([]byte, 6)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// migrateProfiles: state.json from 0.1/0.2.0 has no list; turn the active source into the first entry.
func (a *App) migrateProfiles() {
	if len(a.state.Profiles) > 0 || a.state.Source == "" {
		return
	}
	p := Profile{ID: newID(), Updated: a.state.LastUpdate}
	switch a.state.Source {
	case "url":
		if a.state.SubURL == "" {
			return
		}
		p.Kind, p.URL, p.Name = "url", a.state.SubURL, defaultURLName(a.state.SubURL)
	case "file":
		p.Kind, p.Name = "file", "Local YAML"
	default:
		return
	}
	a.state.Profiles = []Profile{p}
	a.state.ActiveID = p.ID
	a.saveState()
}

func defaultURLName(u string) string {
	if pu, err := url.Parse(u); err == nil && pu.Hostname() != "" {
		return pu.Hostname()
	}
	return "Subscription"
}

func cleanName(n string) string {
	n = strings.TrimSpace(strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, n))
	if r := []rune(n); len(r) > 64 {
		n = string(r[:64])
	}
	return n
}

func (a *App) findProfile(id string) int {
	for i, p := range a.state.Profiles {
		if p.ID == id {
			return i
		}
	}
	return -1
}

// upsertProfile records a successful import and makes it the active entry. Same URL / same path = same entry.
func (a *App) upsertProfile(p Profile) Profile {
	p.Name = cleanName(p.Name)
	p.Updated = a.state.LastUpdate
	idx := -1
	for i, q := range a.state.Profiles {
		if q.Kind == p.Kind && ((p.Kind == "url" && q.URL == p.URL) || (p.Kind == "file" && q.Path == p.Path)) {
			idx = i
			break
		}
	}
	if idx >= 0 {
		q := a.state.Profiles[idx]
		if p.Name == "" {
			p.Name = q.Name
		}
		p.ID = q.ID
		a.state.Profiles[idx] = p
	} else {
		if p.ID == "" {
			p.ID = newID()
		}
		if p.Name == "" {
			switch {
			case p.Kind == "url":
				p.Name = defaultURLName(p.URL)
			case p.Path != "":
				p.Name = filepath.Base(p.Path)
			default:
				p.Name = "Local YAML"
			}
		}
		a.state.Profiles = append(a.state.Profiles, p)
		if len(a.state.Profiles) > maxProfiles {
			// drop the oldest inactive one
			for i, q := range a.state.Profiles {
				if q.ID != a.state.ActiveID && q.ID != p.ID {
					a.state.Profiles = append(a.state.Profiles[:i], a.state.Profiles[i+1:]...)
					break
				}
			}
		}
	}
	a.state.ActiveID = p.ID
	a.saveState()
	return p
}

// resolveUserPath turns what was typed in VR ("~/x.yaml", "Downloads/x.yaml", "/home/steamos/x.yaml") into a
// real path and refuses anything outside the home directory (symlinks resolved first) or not a regular file.
func resolveUserPath(home, typed string) (string, error) {
	typed = strings.TrimSpace(typed)
	if typed == "" {
		return "", errors.New("no file path given")
	}
	if home == "" {
		return "", errors.New("home directory unknown")
	}
	switch {
	case typed == "~":
		typed = home
	case strings.HasPrefix(typed, "~/"):
		typed = filepath.Join(home, typed[2:])
	case !filepath.IsAbs(typed):
		typed = filepath.Join(home, typed)
	}
	realHome, err := filepath.EvalSymlinks(home)
	if err != nil {
		realHome = filepath.Clean(home)
	}
	p, err := filepath.EvalSymlinks(filepath.Clean(typed))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return "", fmt.Errorf("file not found: %s", typed)
		}
		return "", err
	}
	if !strings.HasPrefix(p, realHome+string(filepath.Separator)) {
		return "", fmt.Errorf("only files under %s can be imported", home)
	}
	st, err := os.Stat(p)
	if err != nil {
		return "", err
	}
	if !st.Mode().IsRegular() {
		return "", fmt.Errorf("not a regular file: %s", typed)
	}
	if st.Size() > maxImportBytes {
		return "", fmt.Errorf("file too large (%d bytes, max 32 MB)", st.Size())
	}
	return p, nil
}

func readUserFile(home, typed string) (string, []byte, error) {
	p, err := resolveUserPath(home, typed)
	if err != nil {
		return "", nil, err
	}
	b, err := os.ReadFile(p)
	return p, b, err
}

type yamlFile struct {
	Name     string `json:"name"`
	Path     string `json:"path"` // as shown / sent back: "~/..." when under home
	Size     int64  `json:"size"`
	Modified string `json:"modified"`
}

// listYAML: candidate files for the panel's "pick a file" list: ~/.config/frameclash/profiles/*.y{a,}ml (minus our own
// working copies) and ~/Downloads/*.y{a,}ml (top level only, where a browser saves). Newest first, at most 40.
func (a *App) listYAML() []yamlFile {
	out := []yamlFile{}
	dirs := []string{a.p("profiles")}
	if a.home != "" {
		dirs = append(dirs, filepath.Join(a.home, "Downloads"))
	}
	own := map[string]bool{"subscription.yaml": true, "local.yaml": true}
	for _, d := range dirs {
		ents, err := os.ReadDir(d)
		if err != nil {
			continue
		}
		for _, e := range ents {
			n := e.Name()
			low := strings.ToLower(n)
			if (!strings.HasSuffix(low, ".yaml") && !strings.HasSuffix(low, ".yml")) || strings.HasPrefix(n, ".") {
				continue
			}
			if d == a.p("profiles") && own[n] {
				continue
			}
			full := filepath.Join(d, n)
			if _, err := resolveUserPath(a.home, full); err != nil {
				continue // e.g. a symlink pointing outside home, a directory, >32 MB
			}
			st, err := os.Stat(full)
			if err != nil {
				continue
			}
			out = append(out, yamlFile{Name: n, Path: a.tildePath(full), Size: st.Size(), Modified: st.ModTime().Format(time.RFC3339)})
		}
	}
	sort.SliceStable(out, func(i, j int) bool { return out[i].Modified > out[j].Modified })
	if len(out) > 40 {
		out = out[:40]
	}
	return out
}

func (a *App) tildePath(p string) string {
	if a.home != "" && strings.HasPrefix(p, a.home+"/") {
		return "~/" + p[len(a.home)+1:]
	}
	return p
}

func (a *App) hProfiles(w http.ResponseWriter, r *http.Request) {
	a.mu.Lock()
	defer a.mu.Unlock()
	list := []map[string]any{}
	for _, p := range a.state.Profiles {
		e := map[string]any{"id": p.ID, "name": p.Name, "kind": p.Kind, "updated": p.Updated, "active": p.ID == a.state.ActiveID}
		if p.Kind == "url" {
			e["url"] = maskURL(p.URL)
		} else {
			e["path"] = a.tildePath(p.Path)
		}
		list = append(list, e)
	}
	jsonOut(w, 200, map[string]any{"active": a.state.ActiveID, "profiles": list, "files": a.listYAML(),
		"dir": a.tildePath(a.p("profiles"))})
}

// hLocalPath: import a YAML file the user named by path (typed in VR or picked from the list).
func (a *App) hLocalPath(w http.ResponseWriter, r *http.Request) {
	var in struct{ Path, Name string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	real, data, err := readUserFile(a.home, in.Path)
	if err != nil {
		errOut(w, err)
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	p, err := a.importBytes(Profile{Kind: "file", Path: real, Name: in.Name}, data)
	if err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]any{"ok": true, "id": p.ID, "name": p.Name})
}

// activate makes a saved entry active. It is also "refresh": the URL is downloaded again / the file read again.
func (a *App) activate(id string) (Profile, error) {
	a.mu.Lock()
	i := a.findProfile(id)
	if i < 0 {
		a.mu.Unlock()
		return Profile{}, errors.New("no such profile")
	}
	p := a.state.Profiles[i]
	ua := a.userAgent()
	a.mu.Unlock()
	// read/downloaded without the lock: /api/status keeps answering during a slow download
	var data []byte
	var err error
	switch {
	case p.Kind == "url":
		if data, err = download(p.URL, ua); err != nil {
			err = fmt.Errorf("download failed: %w", err)
		}
	case p.Path != "":
		_, data, err = readUserFile(a.home, p.Path)
	default:
		data, err = os.ReadFile(a.p("profiles", "local.yaml"))
	}
	if err != nil {
		return p, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.importBytes(p, data)
}

func (a *App) hProfileActivate(w http.ResponseWriter, r *http.Request) {
	var in struct{ ID string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	p, err := a.activate(in.ID)
	if err != nil {
		errOut(w, err)
		return
	}
	jsonOut(w, 200, map[string]any{"ok": true, "id": p.ID, "name": p.Name})
}

// hProfileDelete: forget a saved entry (not the active one). Never deletes the user's file.
func (a *App) hProfileDelete(w http.ResponseWriter, r *http.Request) {
	var in struct{ ID string }
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	i := a.findProfile(in.ID)
	if i < 0 {
		errOut(w, errors.New("no such profile"))
		return
	}
	if in.ID == a.state.ActiveID {
		errOut(w, errors.New("the active profile can't be removed; use another one first"))
		return
	}
	a.state.Profiles = append(a.state.Profiles[:i], a.state.Profiles[i+1:]...)
	a.saveState()
	jsonOut(w, 200, map[string]bool{"ok": true})
}
