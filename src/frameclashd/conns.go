package main

import (
	"cmp"
	"encoding/json/v2"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"slices"
	"strconv"
	"time"
)

type conn struct {
	ID          string       `json:"id"`
	Upload      int64        `json:"upload"`
	Download    int64        `json:"download"`
	Start       time.Time    `json:"start"`
	Chains      []string     `json:"chains"`
	Rule        string       `json:"rule"`
	RulePayload string       `json:"rulePayload"`
	Metadata    connMetadata `json:"metadata"`
}

type connMetadata struct {
	Network         string `json:"network"`
	Type            string `json:"type"`
	Host            string `json:"host"`
	SniffHost       string `json:"sniffHost"`
	DestinationIP   string `json:"destinationIP"`
	DestinationPort string `json:"destinationPort"`
	Process         string `json:"process"`
	ProcessPath     string `json:"processPath"`
}

type connsResp struct {
	DownloadTotal int64  `json:"downloadTotal"`
	UploadTotal   int64  `json:"uploadTotal"`
	Memory        int64  `json:"memory"`
	Connections   []conn `json:"connections"`
}

type connGroup struct {
	Key      string `json:"key"`
	Count    int    `json:"count"`
	Upload   int64  `json:"upload"`
	Download int64  `json:"download"`
	OldestS  int64  `json:"oldest_s"`
}

type connEntry struct {
	ID       string `json:"id"`
	Network  string `json:"network"`
	Type     string `json:"type"`
	Host     string `json:"host"`
	Port     int    `json:"port"`
	Process  string `json:"process"`
	Rule     string `json:"rule"`
	Chain    string `json:"chain"`
	Group    string `json:"group"`
	Upload   int64  `json:"upload"`
	Download int64  `json:"download"`
	AgeS     int64  `json:"age_s"`
}

func (a *App) fetchConns() (connsResp, error) {
	var in connsResp
	resp, err := a.ctl("GET", "/connections", nil)
	if err != nil {
		return in, fmt.Errorf("mihomo controller: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return in, fmt.Errorf("mihomo controller: HTTP %d", resp.StatusCode)
	}
	if err := json.UnmarshalRead(io.LimitReader(resp.Body, 64<<20), &in); err != nil {
		return in, err
	}
	return in, nil
}

func connHost(c conn) string {
	if c.Metadata.Host != "" {
		return c.Metadata.Host
	}
	if c.Metadata.SniffHost != "" {
		return c.Metadata.SniffHost
	}
	if c.Metadata.DestinationIP != "" {
		return c.Metadata.DestinationIP
	}
	return "?"
}

func connRule(c conn) string {
	rule := c.Rule
	if c.RulePayload != "" {
		rule += "," + c.RulePayload
	}
	if rule == "" {
		return "?"
	}
	return rule
}

func connChain(c conn) string {
	if len(c.Chains) == 0 || c.Chains[0] == "" {
		return "?"
	}
	return c.Chains[0]
}

func connKey(c conn, by string) string {
	switch by {
	case "process":
		if c.Metadata.Process != "" {
			return c.Metadata.Process
		}
	case "host":
		return connHost(c)
	case "rule":
		return connRule(c)
	case "chain":
		return connChain(c)
	}
	return "?"
}

func ageSeconds(start, now time.Time) int64 {
	if start.IsZero() || start.After(now) {
		return 0
	}
	return int64(now.Sub(start).Seconds())
}

func groupConns(cs []conn, by string, now time.Time) []connGroup {
	groups := make([]connGroup, 0)
	oldest := make([]time.Time, 0)
	index := map[string]int{}
	for _, c := range cs {
		key := connKey(c, by)
		i, ok := index[key]
		if !ok {
			i = len(groups)
			index[key] = i
			groups = append(groups, connGroup{Key: key})
			oldest = append(oldest, c.Start)
		}
		groups[i].Count++
		groups[i].Upload += c.Upload
		groups[i].Download += c.Download
		if !c.Start.IsZero() && (oldest[i].IsZero() || c.Start.Before(oldest[i])) {
			oldest[i] = c.Start
		}
	}
	for i := range groups {
		groups[i].OldestS = ageSeconds(oldest[i], now)
	}
	slices.SortFunc(groups, func(a, b connGroup) int {
		if n := cmp.Compare(b.Count, a.Count); n != 0 {
			return n
		}
		return cmp.Compare(a.Key, b.Key)
	})
	return groups
}

func listConns(cs []conn, by, key string, now time.Time) []connEntry {
	type timedEntry struct {
		entry connEntry
		start time.Time
	}
	timed := make([]timedEntry, 0, len(cs))
	for _, c := range cs {
		if key != "" && (by == "none" || connKey(c, by) != key) {
			continue
		}
		port, _ := strconv.Atoi(c.Metadata.DestinationPort)
		group := "?"
		if len(c.Chains) > 0 && c.Chains[len(c.Chains)-1] != "" {
			group = c.Chains[len(c.Chains)-1]
		}
		timed = append(timed, timedEntry{
			entry: connEntry{
				ID: c.ID, Network: c.Metadata.Network, Type: c.Metadata.Type, Host: connHost(c), Port: port,
				Process: c.Metadata.Process, Rule: connRule(c), Chain: connChain(c), Group: group,
				Upload: c.Upload, Download: c.Download, AgeS: ageSeconds(c.Start, now),
			},
			start: c.Start,
		})
	}
	slices.SortFunc(timed, func(a, b timedEntry) int {
		if n := b.start.Compare(a.start); n != 0 {
			return n
		}
		return cmp.Compare(a.entry.ID, b.entry.ID)
	})
	entries := make([]connEntry, len(timed))
	for i := range timed {
		entries[i] = timed[i].entry
	}
	return entries
}

func validConnBy(by string, allowNone bool) bool {
	return slices.Contains([]string{"process", "host", "rule", "chain"}, by) || allowNone && by == "none"
}

func connLimit(r *http.Request) (int, error) {
	limit := 50
	if raw := r.URL.Query().Get("limit"); raw != "" {
		n, err := strconv.Atoi(raw)
		if err != nil || n < 1 {
			return 0, errors.New("limit must be a positive integer")
		}
		limit = min(n, 200)
	}
	return limit, nil
}

func (a *App) hConnections(w http.ResponseWriter, r *http.Request) {
	in, err := a.fetchConns()
	if err != nil {
		errOut(w, err)
		return
	}
	by := r.URL.Query().Get("by")
	if by == "" {
		by = "process"
	}
	if !validConnBy(by, true) {
		errOut(w, errors.New("by must be process, host, rule, chain or none"))
		return
	}
	limit, err := connLimit(r)
	if err != nil {
		errOut(w, err)
		return
	}
	key := r.URL.Query().Get("key")
	out := map[string]any{
		"total": len(in.Connections), "upload_total": in.UploadTotal,
		"download_total": in.DownloadTotal, "memory": in.Memory,
	}
	if by != "none" && key == "" {
		groups := groupConns(in.Connections, by, time.Now())
		groups = groups[:min(limit, len(groups))]
		out["groups"] = groups
	} else {
		entries := listConns(in.Connections, by, key, time.Now())
		out["matched"] = len(entries)
		entries = entries[:min(limit, len(entries))]
		out["connections"] = entries
	}
	jsonOut(w, http.StatusOK, out)
}

func (a *App) deleteConnections(path string) error {
	resp, err := a.ctl("DELETE", path, nil)
	if err != nil {
		return fmt.Errorf("mihomo controller: %w", err)
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusNoContent {
		return fmt.Errorf("mihomo controller: HTTP %d", resp.StatusCode)
	}
	return nil
}

func (a *App) hCloseConns(w http.ResponseWriter, r *http.Request) {
	var in struct {
		All bool   `json:"all"`
		ID  string `json:"id"`
		By  string `json:"by"`
		Key string `json:"key"`
	}
	if err := readJSON(r, &in); err != nil {
		errOut(w, err)
		return
	}
	modes := 0
	for _, set := range []bool{in.All, in.ID != "", in.By != "" || in.Key != ""} {
		if set {
			modes++
		}
	}
	if modes != 1 || in.By != "" && !validConnBy(in.By, false) || in.By == "" && in.Key != "" ||
		in.By != "" && in.Key == "" {
		errOut(w, errors.New("specify exactly one of all, id, or by and key"))
		return
	}
	closed := 0
	if in.ID != "" {
		if err := a.deleteConnections("/connections/" + url.PathEscape(in.ID)); err != nil {
			errOut(w, err)
			return
		}
		closed = 1
	} else {
		fetched, err := a.fetchConns()
		if err != nil {
			errOut(w, err)
			return
		}
		if in.All {
			closed = len(fetched.Connections)
			err = a.deleteConnections("/connections")
		} else {
			ids := make([]string, 0)
			for _, c := range fetched.Connections {
				if connKey(c, in.By) == in.Key {
					ids = append(ids, c.ID)
				}
			}
			for _, id := range ids {
				if err := a.deleteConnections("/connections/" + url.PathEscape(id)); err != nil {
					errOut(w, err)
					return
				}
				closed++
			}
		}
		if err != nil {
			errOut(w, err)
			return
		}
	}
	jsonOut(w, http.StatusOK, map[string]any{"ok": true, "closed": closed})
}
