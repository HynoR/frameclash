// FrameClash panel: a tiny HTTP/1.0 client for frameclashd's loopback API, and the parsed data the panel shows.
// Copyright (c) 2026 FrameClash contributors. MIT License.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct HttpResult {
    bool ok = false;      ///< got an HTTP response at all
    int status = 0;
    std::string body;
    std::string error;    ///< transport error (connect refused, timeout, ...)
};

/** One request to 127.0.0.1:port (HTTP/1.0, Connection: close, X-FrameClash: 1). */
HttpResult httpRequest(int port, const std::string& method, const std::string& path, const std::string& body,
                       int timeoutMs);

struct DaemonStatus {
    std::string version;
    bool running = false;
    int pid = 0;
    bool enabled = false;
    std::string source;      ///< "", "url", "file"
    std::string subUrl;      ///< masked by frameclashd (scheme://host/…)
    std::string lastUpdate;
    std::string error;
    std::string proxy;
    int mixedPort = 0;
    bool tun = false;        ///< 0.3.0: TUN mode on (applies while running)
    std::string tunReady;    ///< "" = the TUN helper is set up, else why not
    bool selfDns = true;     ///< TUN: point systemd-resolved into the tun (preference)
    std::string tunDns;      ///< TUN running: "on", "off", "no-permission", "" (starting) or an error
    std::string mode;        ///< 0.5.0: outbound mode "rule", "global" or "direct" ("" = older daemon)
};

struct ProxyNode {
    std::string name;
    std::string type;               ///< Shadowsocks, Vmess, Direct, Selector, URLTest, ...
    std::string now;                ///< for groups
    std::vector<std::string> all;   ///< for groups
    int delay = -1;                 ///< last test: -1 never tested, 0 failed/timeout, >0 ms
    bool isGroup() const;
};

struct ConnSummary {
    bool have = false;
    int connections = 0;
    int64_t upload = 0;
    int64_t download = 0;
};

/** One saved profile from /api/profiles (0.2.1). URLs arrive masked; the panel never sees a full subscription URL. */
struct ProfileEntry {
    std::string id;
    std::string name;
    std::string kind;      ///< "url" or "file"
    std::string where;     ///< masked URL or "~/..." path ("" = YAML pasted into the web UI)
    std::string updated;
    bool active = false;
};

/** /api/exitip: where proxied traffic leaves to the internet (0.5.0, checked on demand). */
struct ExitIp {
    std::string ip, country, city, org;
};

/** Parse /api/exitip. */
bool parseExitIp(const std::string& json, ExitIp& out);

/** A YAML file frameclashd found in ~/.config/frameclash/profiles or ~/Downloads. */
struct YamlFile {
    std::string name;
    std::string path;      ///< "~/..." (sent back as-is to /api/local/path)
    int64_t size = 0;
    std::string modified;
};

struct ProfilesInfo {
    std::vector<ProfileEntry> profiles;
    std::vector<YamlFile> files;
    std::string dir;       ///< "~/.config/frameclash/profiles"
};

/** Parse /api/profiles. */
bool parseProfiles(const std::string& json, ProfilesInfo& out, std::string& error);

// ---- text field editing (keyboard input), kept here so the unit tests can reach it ----
/** Remove the last UTF-8 character (not just the last byte). */
void utf8PopBack(std::string& s);
/** Strip control characters and surrounding whitespace (for a full-text replace / paste). */
std::string cleanFieldText(const std::string& s, size_t maxBytes);

/** Parse /api/status. */
bool parseStatus(const std::string& json, DaemonStatus& out, std::string& error);
/** Parse /api/proxies (mihomo /proxies) into nodes by name and the group order to show. */
bool parseProxies(const std::string& json, std::map<std::string, ProxyNode>& nodes, std::vector<std::string>& groups,
                  std::string& error);
/** Parse /api/groupdelay (mihomo /group/x/delay: {"name": ms, ...}). */
bool parseGroupDelay(const std::string& json, std::map<std::string, int>& out, std::string& error);
/** Parse /api/delay (mihomo /proxies/x/delay: {"delay": ms} or {"message": ...}). */
int parseNodeDelay(const std::string& json);
bool parseConn(const std::string& json, ConnSummary& out);
/** The error text of a failed API call ({"error": ...} / {"message": ...} / plain text). */
std::string apiErrorText(const HttpResult& result);
/** JSON-escape a string (with the quotes). */
std::string jsonString(const std::string& s);
/** Follow a group's `now` through nested groups to the leaf node (max 6 hops). */
std::string resolveLeaf(const std::map<std::string, ProxyNode>& nodes, const std::string& group);
