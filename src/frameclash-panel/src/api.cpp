// Copyright (c) 2026 FrameClash contributors. MIT License.
#include "api.h"

#include "third_party_frameeyeosc/json.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

int msLeft(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

/** Decode a chunked body (frameclashd answers HTTP/1.0 without chunking; this is only a safety net). */
std::string dechunk(const std::string& raw) {
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) break;
        const size_t len = std::strtoul(raw.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (len == 0) break;
        pos = eol + 2;
        out.append(raw, pos, len);
        pos += len + 2;
    }
    return out;
}

const JsonValue* member(const JsonValue& v, const char* key) {
    return v.get(key);
}

std::string str(const JsonValue& v, const char* key) {
    const JsonValue* m = v.get(key);
    return m != nullptr && m->isString() ? m->text : std::string();
}

double num(const JsonValue& v, const char* key, double fallback = 0) {
    const JsonValue* m = v.get(key);
    return m != nullptr && m->isNumber() ? m->number : fallback;
}

bool boolean(const JsonValue& v, const char* key) {
    const JsonValue* m = v.get(key);
    return m != nullptr && m->isBool() && m->boolean;
}

}  // namespace

HttpResult httpRequest(int port, const std::string& method, const std::string& path, const std::string& body,
                       int timeoutMs) {
    HttpResult result;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        result.error = std::strerror(errno);
        return result;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only, always
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS) {
        result.error = std::strerror(errno);
        ::close(fd);
        return result;
    }
    pollfd pfd {fd, POLLOUT, 0};
    if (::poll(&pfd, 1, msLeft(deadline)) <= 0) {
        result.error = "connect timed out";
        ::close(fd);
        return result;
    }
    int soError = 0;
    socklen_t len = sizeof(soError);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
    if (soError != 0) {
        result.error = std::strerror(soError);
        ::close(fd);
        return result;
    }
    std::string request = method + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nUser-Agent: frameclash-panel\r\nX-FrameClash: 1\r\nConnection: close\r\n";
    if (method != "GET") {
        request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    if (method != "GET") request += body;
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            pfd = {fd, POLLOUT, 0};
            if (::poll(&pfd, 1, msLeft(deadline)) > 0) continue;
        }
        result.error = "send failed";
        ::close(fd);
        return result;
    }
    std::string raw;
    char buffer[16384];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            raw.append(buffer, static_cast<size_t>(n));
            if (raw.size() > (64u << 20)) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EINTR) {
            pfd = {fd, POLLIN, 0};
            const int left = msLeft(deadline);
            if (left > 0 && ::poll(&pfd, 1, left) > 0) continue;
            result.error = "timed out";
            ::close(fd);
            return result;
        }
        result.error = std::strerror(errno);
        ::close(fd);
        return result;
    }
    ::close(fd);
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (raw.compare(0, 5, "HTTP/") != 0 || headerEnd == std::string::npos) {
        result.error = "bad HTTP response";
        return result;
    }
    const size_t space = raw.find(' ');
    result.status = std::atoi(raw.c_str() + space + 1);
    const std::string headers = lower(raw.substr(0, headerEnd));
    result.body = raw.substr(headerEnd + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) result.body = dechunk(result.body);
    result.ok = true;
    return result;
}

bool ProxyNode::isGroup() const {
    return type == "Selector" || type == "URLTest" || type == "Fallback" || type == "LoadBalance" ||
           type == "Relay";
}

bool parseStatus(const std::string& json, DaemonStatus& out, std::string& error) {
    JsonValue v;
    if (!parseJson(json, v, error)) return false;
    if (!v.isObject()) {
        error = "status is not an object";
        return false;
    }
    out.version = str(v, "version");
    out.running = boolean(v, "running");
    out.pid = static_cast<int>(num(v, "pid"));
    out.enabled = boolean(v, "enabled");
    out.source = str(v, "source");
    out.subUrl = str(v, "sub_url");
    out.lastUpdate = str(v, "last_update");
    out.error = str(v, "error");
    out.proxy = str(v, "proxy");
    out.mixedPort = static_cast<int>(num(v, "mixed_port"));
    out.tun = boolean(v, "tun");
    out.tunReady = str(v, "tun_ready");
    out.selfDns = boolean(v, "self_dns");
    out.tunDns = str(v, "tun_dns");
    out.mode = str(v, "mode");
    return true;
}

bool parseProxies(const std::string& json, std::map<std::string, ProxyNode>& nodes, std::vector<std::string>& groups,
                  std::string& error) {
    JsonValue v;
    if (!parseJson(json, v, error)) return false;
    const JsonValue* proxies = v.get("proxies");
    if (proxies == nullptr || !proxies->isObject()) {
        error = "no \"proxies\" in the answer";
        return false;
    }
    nodes.clear();
    groups.clear();
    for (const auto& [name, p] : proxies->members) {
        ProxyNode node;
        node.name = name;
        node.type = str(p, "type");
        node.now = str(p, "now");
        if (const JsonValue* all = member(p, "all"); all != nullptr && all->isArray()) {
            for (const JsonValue& item : all->items)
                if (item.isString()) node.all.push_back(item.text);
        }
        if (const JsonValue* history = member(p, "history"); history != nullptr && history->isArray() &&
                                                              !history->items.empty()) {
            node.delay = static_cast<int>(num(history->items.back(), "delay", -1));
        }
        nodes[name] = node;
    }
    // Same order as the web UI: GLOBAL's list (the config order), groups only; GLOBAL itself only if alone
    std::vector<std::string> order;
    auto global = nodes.find("GLOBAL");
    if (global != nodes.end() && !global->second.all.empty()) {
        order = global->second.all;
    } else {
        for (const auto& [name, node] : nodes) order.push_back(name);
    }
    for (const std::string& name : order) {
        auto it = nodes.find(name);
        if (it != nodes.end() && it->second.isGroup() && name != "GLOBAL") groups.push_back(name);
    }
    if (groups.empty() && global != nodes.end()) groups.push_back("GLOBAL");
    return true;
}

bool parseGroupDelay(const std::string& json, std::map<std::string, int>& out, std::string& error) {
    JsonValue v;
    if (!parseJson(json, v, error)) return false;
    if (!v.isObject()) {
        error = "not an object";
        return false;
    }
    if (const JsonValue* message = v.get("message"); message != nullptr && message->isString() &&
                                                      v.members.size() == 1) {
        error = message->text;
        return false;
    }
    for (const auto& [name, d] : v.members)
        if (d.isNumber()) out[name] = static_cast<int>(d.number);
    return true;
}

int parseNodeDelay(const std::string& json) {
    JsonValue v;
    std::string error;
    if (!parseJson(json, v, error) || !v.isObject()) return 0;
    return static_cast<int>(num(v, "delay", 0));
}

bool parseConn(const std::string& json, ConnSummary& out) {
    JsonValue v;
    std::string error;
    if (!parseJson(json, v, error) || !v.isObject() || v.get("connections") == nullptr) return false;
    out.have = true;
    out.connections = static_cast<int>(num(v, "connections"));
    out.upload = static_cast<int64_t>(num(v, "upload_total"));
    out.download = static_cast<int64_t>(num(v, "download_total"));
    return true;
}

bool parseExitIp(const std::string& json, ExitIp& out) {
    JsonValue v;
    std::string error;
    if (!parseJson(json, v, error) || !v.isObject()) return false;
    out = {str(v, "ip"), str(v, "country"), str(v, "city"), str(v, "org")};
    return !out.ip.empty();
}

std::string apiErrorText(const HttpResult& result) {
    if (!result.ok) return result.error;
    JsonValue v;
    std::string error;
    if (parseJson(result.body, v, error) && v.isObject()) {
        const std::string e = str(v, "error");
        if (!e.empty()) return e;
        const std::string m = str(v, "message");
        if (!m.empty()) return m;
    }
    std::string text = result.body;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.size() > 200) text = text.substr(0, utf8Prefix(text, 200)) + "…";
    return text.empty() ? "HTTP " + std::to_string(result.status) : text;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string resolveLeaf(const std::map<std::string, ProxyNode>& nodes, const std::string& group) {
    std::string name = group;
    for (int hop = 0; hop < 6; ++hop) {
        auto it = nodes.find(name);
        if (it == nodes.end() || !it->second.isGroup() || it->second.now.empty()) return name;
        name = it->second.now;
    }
    return name;
}

bool parseProfiles(const std::string& json, ProfilesInfo& out, std::string& error) {
    JsonValue v;
    if (!parseJson(json, v, error)) return false;
    if (!v.isObject()) {
        error = "profiles is not an object";
        return false;
    }
    out = {};
    out.dir = str(v, "dir");
    if (const JsonValue* list = v.get("profiles"); list != nullptr && list->isArray()) {
        for (const JsonValue& p : list->items) {
            if (!p.isObject()) continue;
            ProfileEntry e;
            e.id = str(p, "id");
            e.name = str(p, "name");
            e.kind = str(p, "kind");
            e.where = e.kind == "url" ? str(p, "url") : str(p, "path");
            e.updated = str(p, "updated");
            e.active = boolean(p, "active");
            if (!e.id.empty()) out.profiles.push_back(e);
        }
    }
    if (const JsonValue* list = v.get("files"); list != nullptr && list->isArray()) {
        for (const JsonValue& f : list->items) {
            if (!f.isObject()) continue;
            YamlFile y;
            y.name = str(f, "name");
            y.path = str(f, "path");
            y.size = static_cast<int64_t>(num(f, "size"));
            y.modified = str(f, "modified");
            if (!y.path.empty()) out.files.push_back(y);
        }
    }
    return true;
}

void utf8PopBack(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;  // stop after removing a lead byte (or an ASCII byte)
    }
}

std::string cleanFieldText(const std::string& s, size_t maxBytes) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            out += ' ';
        } else if (c >= 0x20 && c != 0x7f) {
            out += static_cast<char>(c);
        }
    }
    const size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    out = out.substr(a, out.find_last_not_of(' ') - a + 1);
    if (out.size() > maxBytes) out.resize(utf8Prefix(out, maxBytes));
    return out;
}
