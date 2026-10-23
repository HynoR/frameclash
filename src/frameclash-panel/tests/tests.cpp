// Unit tests for the API parsing (no network, no SteamVR). Copyright (c) 2026 FrameClash contributors. MIT License.
#include "api.h"
#include "spawn.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    std::string error;
    DaemonStatus st;
    CHECK(parseStatus(R"({"version":"0.2.0","running":true,"pid":12,"source":"url","proxy":"127.0.0.1:7890",)"
                      R"("mixed_port":7890,"last_update":"2026-10-23T17:00:00+08:00","error":"","mode":"global"})",
                      st, error));
    CHECK(st.running && st.pid == 12 && st.source == "url" && st.mixedPort == 7890 && st.version == "0.2.0");
    CHECK(st.mode == "global");
    ExitIp ip;
    CHECK(parseExitIp(R"({"ip":"203.0.113.9","country":"HK","city":"Hong Kong","org":"AS1 X"})", ip));
    CHECK(ip.ip == "203.0.113.9" && ip.country == "HK");
    CHECK(!parseExitIp(R"({"error":"the proxy is not running"})", ip));
    CHECK(!parseStatus("not json", st, error));

    std::map<std::string, ProxyNode> nodes;
    std::vector<std::string> groups;
    const char* proxies = R"({"proxies":{
      "GLOBAL":{"type":"Selector","now":"节点选择","all":["DIRECT","节点选择","自动选择","香港 01"]},
      "节点选择":{"type":"Selector","now":"自动选择","all":["自动选择","香港 01","DIRECT"]},
      "自动选择":{"type":"URLTest","now":"香港 01","all":["香港 01"],"history":[{"delay":120}]},
      "香港 01":{"type":"Shadowsocks","history":[{"delay":0},{"delay":88}]},
      "DIRECT":{"type":"Direct","history":[]}}})";
    CHECK(parseProxies(proxies, nodes, groups, error));
    CHECK(groups.size() == 2 && groups[0] == "节点选择" && groups[1] == "自动选择");
    CHECK(nodes["香港 01"].delay == 88);
    CHECK(nodes["DIRECT"].delay == -1);
    CHECK(resolveLeaf(nodes, "节点选择") == "香港 01");

    std::map<std::string, int> d;
    CHECK(parseGroupDelay(R"({"a":10,"b":200})", d, error) && d["a"] == 10 && d.size() == 2);
    CHECK(!parseGroupDelay(R"({"message":"Timeout"})", d, error) && error == "Timeout");
    CHECK(parseNodeDelay(R"({"delay":42})") == 42);
    CHECK(parseNodeDelay(R"({"message":"timeout"})") == 0);

    ConnSummary c;
    CHECK(parseConn(R"({"connections":3,"upload_total":10,"download_total":2048})", c) && c.connections == 3 &&
          c.download == 2048);

    HttpResult r;
    r.ok = true;
    r.status = 400;
    r.body = R"({"error":"no subscription URL saved"})";
    CHECK(apiErrorText(r) == "no subscription URL saved");
    CHECK(jsonString("a\"b\\\n") == R"("a\"b\\\n")");

    // 0.2.1: /api/profiles
    ProfilesInfo pi;
    CHECK(parseProfiles(R"({"active":"b","dir":"~/.config/frameclash/profiles","profiles":[
      {"id":"a","name":"机场","kind":"url","url":"https://example.com/…","updated":"2026-10-23T17:00:00+08:00","active":false},
      {"id":"b","name":"My file","kind":"file","path":"~/Downloads/c.yaml","active":true}],
      "files":[{"name":"c.yaml","path":"~/Downloads/c.yaml","size":2105,"modified":"x"}]})", pi, error));
    CHECK(pi.profiles.size() == 2 && pi.profiles[0].where == "https://example.com/…" && !pi.profiles[0].active);
    CHECK(pi.profiles[1].active && pi.profiles[1].where == "~/Downloads/c.yaml" && pi.profiles[1].kind == "file");
    CHECK(pi.files.size() == 1 && pi.files[0].size == 2105 && pi.dir == "~/.config/frameclash/profiles");
    CHECK(parseProfiles(R"({"profiles":[],"files":null})", pi, error) && pi.profiles.empty() && pi.files.empty());
    CHECK(!parseProfiles("[1]", pi, error));

    // keyboard input -> field
    std::string f = "ab香";
    utf8PopBack(f);
    CHECK(f == "ab");
    utf8PopBack(f);
    utf8PopBack(f);
    utf8PopBack(f);
    CHECK(f.empty());
    f = "日本語";
    utf8PopBack(f);
    CHECK(f == "日本");
    CHECK(cleanFieldText("  https://a.b/c\n ", 100) == "https://a.b/c");
    CHECK(cleanFieldText("a\tb\x01" "c", 100) == "a bc");
    CHECK(cleanFieldText("   ", 100).empty());

    // clipboard: a dead $DISPLAY is skipped (the local X sockets are still tried); the answer is text or an error,
    // never both and never a crash
    {
        ::setenv("DISPLAY", ":97", 1);
        std::string error;
        const std::string text = readClipboard(error);
        CHECK(text.empty() != error.empty());
    }

    std::printf(failures == 0 ? "all tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
