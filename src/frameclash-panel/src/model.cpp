// Copyright (c) 2026 FrameClash contributors. MIT License.
#include "model.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

namespace {

constexpr int kQuickTimeoutMs = 3000;
constexpr int kActionTimeoutMs = 15000;   // frameclashd waits up to ~10 s for mihomo on start
constexpr int kRefreshTimeoutMs = 45000;  // subscription download (30 s in frameclashd) + restart
constexpr int kDelayTimeoutMs = 15000;    // mihomo's own test timeout is 5 s
constexpr int kImportTimeoutMs = 45000;   // download (30 s) + mihomo -t + restart

/** Run a program and wait for it (stdio to /dev/null). @return its exit status, or -1 if it couldn't run */
int runWait(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(null, 1), dup2(null, 2);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Model::Model(int port) : port_(port) {
    queue_.push_back({Op::Kind::Poll, "", ""});
    thread_ = std::thread([this] { loop(); });
}

Model::~Model() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

Snapshot Model::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

bool Model::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && !running_;
}

void Model::push(Op op) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Don't queue the same thing twice (a laser double-click, or polls piling up)
        for (const Op& queued : queue_)
            if (queued.kind == op.kind && queued.a == op.a && queued.b == op.b) return;
        queue_.push_back(std::move(op));
    }
    wake_.notify_all();
}

void Model::poll() { push({Op::Kind::Poll, "", ""}); }
void Model::start() { push({Op::Kind::Start, "", ""}); }
void Model::stop() { push({Op::Kind::Stop, "", ""}); }
void Model::setTun(bool on) { push({Op::Kind::Tun, on ? "true" : "false", ""}); }
void Model::setTunDns(bool on) { push({Op::Kind::TunDns, on ? "true" : "false", ""}); }
void Model::setMode(const std::string& mode) { push({Op::Kind::Mode, mode, ""}); }
void Model::checkExitIp() { push({Op::Kind::ExitIp, "", ""}); }
void Model::repair() { push({Op::Kind::Repair, "", ""}); }
void Model::restartDaemon() { push({Op::Kind::RestartDaemon, "", ""}); }
void Model::refreshSubscription() { push({Op::Kind::Refresh, "", ""}); }
void Model::select(const std::string& group, const std::string& name) { push({Op::Kind::Select, group, name}); }
void Model::testGroup(const std::string& group) { push({Op::Kind::TestGroup, group, ""}); }
void Model::testNode(const std::string& name) { push({Op::Kind::TestNode, name, ""}); }
void Model::importUrl(const std::string& url, const std::string& name) { push({Op::Kind::ImportUrl, url, name}); }
void Model::importFile(const std::string& path, const std::string& name) { push({Op::Kind::ImportFile, path, name}); }
void Model::activateProfile(const std::string& id) { push({Op::Kind::Activate, id, ""}); }
void Model::deleteProfile(const std::string& id) { push({Op::Kind::Delete, id, ""}); }

void Model::finishImport(const HttpResult& r, const std::string& okEn, const std::string& okZh,
                         const std::string& failEn, const std::string& failZh) {
    const bool ok = r.ok && r.status == 200;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++snap_.importSeq;
        snap_.importOk = ok;
    }
    // frameclashd's errors never contain the full URL (download errors say "HTTP 404", "timeout", ...)
    if (ok) setMessage(okEn, okZh, false);
    else setMessage(failEn + apiErrorText(r), failZh + apiErrorText(r), true);
}

void Model::setInterval(int ms) {
    bool faster = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        faster = ms < intervalMs_;
        intervalMs_ = ms;
    }
    if (faster) poll();  // the panel was just opened: show fresh data right away
}

void Model::setBusy(Busy busy, const std::string& group) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.busy = busy;
    snap_.testingGroup = group;
    ++snap_.seq;
}

void Model::setMessage(const std::string& en, const std::string& zh, bool error) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.message = en;
    snap_.messageZh = zh;
    snap_.messageIsError = error;
    snap_.messageAtMs = nowMs();
    ++snap_.seq;
}

void Model::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
        if (queue_.empty()) {
            const int interval = intervalMs_;
            wake_.wait_for(lock, std::chrono::milliseconds(interval), [this] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            if (queue_.empty()) queue_.push_back({Op::Kind::Poll, "", ""});
        }
        Op op = queue_.front();
        queue_.pop_front();
        running_ = true;
        lock.unlock();
        if (op.kind == Op::Kind::Poll) {
            doPoll();
        } else {
            doAction(op);
            doPoll();  // show the action's effect at once
        }
        lock.lock();
        running_ = false;
    }
}

void Model::doPoll() {
    const HttpResult status = httpRequest(port_, "GET", "/api/status", "", kQuickTimeoutMs);
    if (!status.ok || status.status != 200) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string why = status.ok ? "HTTP " + std::to_string(status.status) + ": " + apiErrorText(status)
                                          : status.error;
        if (snap_.reach != Reach::Unreachable || snap_.reachError != why) {
            snap_.reach = Reach::Unreachable;
            snap_.reachError = why;
            snap_.nodes.clear();
            snap_.groups.clear();
            snap_.conn = {};
            ++snap_.seq;
        }
        return;
    }
    DaemonStatus parsed;
    std::string error;
    if (!parseStatus(status.body, parsed, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.reach = Reach::Unreachable;
        snap_.reachError = "bad /api/status: " + error;
        ++snap_.seq;
        return;
    }
    std::map<std::string, ProxyNode> nodes;
    std::vector<std::string> groups;
    ConnSummary conn;
    ProfilesInfo profiles;
    const HttpResult pr = httpRequest(port_, "GET", "/api/profiles", "", kQuickTimeoutMs);
    const bool haveProfiles = pr.ok && pr.status == 200 && parseProfiles(pr.body, profiles, error);
    if (parsed.running) {
        const HttpResult proxies = httpRequest(port_, "GET", "/api/proxies", "", kQuickTimeoutMs);
        if (proxies.ok && proxies.status == 200) parseProxies(proxies.body, nodes, groups, error);
        const HttpResult c = httpRequest(port_, "GET", "/api/conn", "", kQuickTimeoutMs);
        if (c.ok && c.status == 200) parseConn(c.body, conn);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // speeds: the change in mihomo's totals since the last poll (a restart resets them: shown as 0)
    const int64_t now = nowMs();
    if (conn.have && snap_.conn.have && now > lastConnAtMs_) {
        const double dt = static_cast<double>(now - lastConnAtMs_) / 1000;
        snap_.upSpeed = std::max<int64_t>(0, conn.upload - snap_.conn.upload) / dt;
        snap_.downSpeed = std::max<int64_t>(0, conn.download - snap_.conn.download) / dt;
    } else {
        snap_.upSpeed = snap_.downSpeed = 0;
    }
    lastConnAtMs_ = now;
    snap_.reach = Reach::Ok;
    snap_.reachError.clear();
    snap_.status = parsed;
    snap_.nodes = std::move(nodes);
    snap_.groups = std::move(groups);
    snap_.conn = conn;
    snap_.haveProfiles = haveProfiles;
    snap_.profiles = std::move(profiles);
    if (!parsed.running) {
        snap_.testedDelay.clear();
        snap_.exitIp = {};
        snap_.exitIpError.clear();
    }
    ++snap_.seq;
}

void Model::doAction(const Op& op) {
    HttpResult r;
    if (op.kind != Op::Kind::ExitIp && op.kind != Op::Kind::TestGroup && op.kind != Op::Kind::TestNode) {
        std::lock_guard<std::mutex> lock(mutex_);  // the route may change: an old exit IP would mislead
        snap_.exitIp = {};
        snap_.exitIpError.clear();
    }
    switch (op.kind) {
        case Op::Kind::Start:
            setBusy(Busy::Starting);
            r = httpRequest(port_, "POST", "/api/start", "{}", kActionTimeoutMs);
            if (r.ok && r.status == 200) setMessage("Proxy started", "代理已启动", false);
            else setMessage("Start failed: " + apiErrorText(r), "启动失败：" + apiErrorText(r), true);
            break;
        case Op::Kind::Stop:
            setBusy(Busy::Stopping);
            r = httpRequest(port_, "POST", "/api/stop", "{}", kActionTimeoutMs);
            if (r.ok && r.status == 200) setMessage("Proxy stopped", "代理已停止", false);
            else setMessage("Stop failed: " + apiErrorText(r), "停止失败：" + apiErrorText(r), true);
            break;
        case Op::Kind::Tun:
            setBusy(Busy::SwitchingTun);
            r = httpRequest(port_, "POST", "/api/tun", "{\"enable\":" + op.a + "}", kActionTimeoutMs);
            if (r.ok && r.status == 200)
                setMessage(op.a == "true" ? "TUN on: all wlan0 traffic goes through the proxy" : "TUN off",
                           op.a == "true" ? "TUN 已开启：wlan0 流量全部走代理（串流与局域网直连）" : "TUN 已关闭", false);
            else setMessage("TUN: " + apiErrorText(r), "TUN：" + apiErrorText(r), true);
            break;
        case Op::Kind::TunDns:
            setBusy(Busy::SwitchingTun);
            r = httpRequest(port_, "POST", "/api/tun/dns", "{\"enable\":" + op.a + "}", kActionTimeoutMs);
            if (r.ok && r.status == 200)
                setMessage(op.a == "true" ? "DNS on: all lookups go through the tun"
                                          : "DNS off: system DNS as the network set it",
                           op.a == "true" ? "DNS 已接管：所有解析走 TUN" : "DNS 已还原：使用网络自带 DNS", false);
            else setMessage("DNS: " + apiErrorText(r), "DNS：" + apiErrorText(r), true);
            break;
        case Op::Kind::Refresh:
            setBusy(Busy::Refreshing);
            r = httpRequest(port_, "POST", "/api/subscription/refresh", "{}", kRefreshTimeoutMs);
            if (r.ok && r.status == 200) setMessage("Subscription updated", "订阅已更新", false);
            else setMessage("Refresh failed: " + apiErrorText(r), "更新订阅失败：" + apiErrorText(r), true);
            break;
        case Op::Kind::Select:
            setBusy(Busy::Selecting, op.a);
            r = httpRequest(port_, "POST", "/api/select",
                            "{\"group\":" + jsonString(op.a) + ",\"name\":" + jsonString(op.b) + "}", kActionTimeoutMs);
            if (r.ok && r.status == 200) setMessage("Now using " + op.b + " in " + op.a, op.a + " 已切换到 " + op.b, false);
            else setMessage("Select failed: " + apiErrorText(r), "切换失败：" + apiErrorText(r), true);
            break;
        case Op::Kind::TestGroup: {
            setBusy(Busy::Testing, op.a);
            r = httpRequest(port_, "POST", "/api/groupdelay", "{\"group\":" + jsonString(op.a) + "}", kDelayTimeoutMs);
            std::map<std::string, int> delays;
            std::string error;
            if (r.ok && r.status == 200 && parseGroupDelay(r.body, delays, error)) {
                int good = 0;
                size_t total = delays.size();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // mihomo leaves out nodes that failed: mark the rest of the group as timed out
                    auto group = snap_.nodes.find(op.a);
                    if (group != snap_.nodes.end()) {
                        total = group->second.all.size();
                        for (const std::string& name : group->second.all)
                            if (!snap_.nodes.count(name) || !snap_.nodes[name].isGroup()) snap_.testedDelay[name] = 0;
                    }
                    for (const auto& [name, ms] : delays) {
                        snap_.testedDelay[name] = ms;
                        if (ms > 0) ++good;
                    }
                }
                setMessage("Latency test done: " + std::to_string(good) + " of " + std::to_string(total) +
                               " answered",
                           "测速完成：" + std::to_string(good) + " / " + std::to_string(total) + " 个节点可用", false);
            } else {
                const std::string why = r.ok && r.status == 200 ? error : apiErrorText(r);
                // mihomo answers 504 / "Timeout" when every node failed
                if (r.ok && (r.status == 504 || r.status == 408)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto group = snap_.nodes.find(op.a);
                    if (group != snap_.nodes.end())
                        for (const std::string& name : group->second.all) snap_.testedDelay[name] = 0;
                }
                setMessage("Latency test failed: " + why, "测速失败：" + why, true);
            }
            break;
        }
        case Op::Kind::TestNode: {
            setBusy(Busy::Testing, "");
            r = httpRequest(port_, "POST", "/api/delay", "{\"name\":" + jsonString(op.a) + "}", kDelayTimeoutMs);
            const int ms = r.ok && r.status == 200 ? parseNodeDelay(r.body) : 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.testedDelay[op.a] = ms;
            }
            setMessage(op.a + ": " + (ms > 0 ? std::to_string(ms) + " ms" : "timeout"), op.a + "：" + (ms > 0 ? std::to_string(ms) + " ms" : "超时"), ms <= 0);
            break;
        }
        case Op::Kind::ImportUrl: {
            setBusy(Busy::Importing);
            r = httpRequest(port_, "POST", "/api/subscription",
                            "{\"url\":" + jsonString(op.a) + ",\"name\":" + jsonString(op.b) + "}", kImportTimeoutMs);
            finishImport(r, "Subscription imported", "订阅已导入", "Import failed: ", "导入失败：");
            break;
        }
        case Op::Kind::ImportFile: {
            setBusy(Busy::Importing);
            r = httpRequest(port_, "POST", "/api/local/path",
                            "{\"path\":" + jsonString(op.a) + ",\"name\":" + jsonString(op.b) + "}", kImportTimeoutMs);
            finishImport(r, "Config file imported", "配置文件已导入", "Import failed: ", "导入失败：");
            break;
        }
        case Op::Kind::Activate: {
            setBusy(Busy::Activating, op.a);
            r = httpRequest(port_, "POST", "/api/profiles/activate", "{\"id\":" + jsonString(op.a) + "}",
                            kImportTimeoutMs);
            finishImport(r, "Profile loaded", "配置已载入", "Loading the profile failed: ", "载入配置失败：");
            break;
        }
        case Op::Kind::Delete: {
            setBusy(Busy::Deleting, op.a);
            r = httpRequest(port_, "POST", "/api/profiles/delete", "{\"id\":" + jsonString(op.a) + "}", kActionTimeoutMs);
            if (r.ok && r.status == 200) setMessage("Profile removed from the list", "已从列表移除", false);
            else setMessage("Remove failed: " + apiErrorText(r), "移除失败：" + apiErrorText(r), true);
            break;
        }
        case Op::Kind::Mode: {
            setBusy(Busy::SwitchingMode);
            r = httpRequest(port_, "POST", "/api/mode", "{\"mode\":" + jsonString(op.a) + "}", kActionTimeoutMs);
            const std::string zh = op.a == "global" ? "全局" : op.a == "direct" ? "直连" : "规则";
            if (r.ok && r.status == 200) setMessage("Mode: " + op.a, "出站模式：" + zh, false);
            else setMessage("Mode: " + apiErrorText(r), "出站模式：" + apiErrorText(r), true);
            break;
        }
        case Op::Kind::ExitIp: {
            setBusy(Busy::CheckingIp);
            r = httpRequest(port_, "GET", "/api/exitip", "", kActionTimeoutMs);
            ExitIp ip;
            const bool ok = r.ok && r.status == 200 && parseExitIp(r.body, ip);
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.exitIp = ip;
            snap_.exitIpError = ok ? "" : apiErrorText(r);
            ++snap_.seq;
            break;
        }
        case Op::Kind::Repair: {
            setBusy(Busy::Repairing);
            r = httpRequest(port_, "POST", "/api/repair", "{}", kRefreshTimeoutMs);
            if (r.ok && r.status == 200) setMessage("Network repaired. The proxy is off; press Start to use it again.",
                                                    "网络已修复。代理已关闭，需要时再点「启动」。", false);
            else setMessage("Repair failed: " + apiErrorText(r), "修复失败：" + apiErrorText(r), true);
            break;
        }
        case Op::Kind::RestartDaemon: {
            setBusy(Busy::Restarting);
            runWait({"systemctl", "--user", "reset-failed", "frameclash"});  // a failed unit may refuse to restart
            const int rc = runWait({"systemctl", "--user", "restart", "frameclash"});
            if (rc == 0) {
                std::this_thread::sleep_for(std::chrono::seconds(2));  // let it bind its port before the next poll
                setMessage("Restarted frameclashd", "已重启后台服务", false);
            } else {
                const std::string why = rc < 0 ? "could not run systemctl" : "systemctl exit code " + std::to_string(rc);
                setMessage("Restart failed (" + why + "). Over SSH: systemctl --user status frameclash",
                           "重启失败（" + why + "）。SSH：systemctl --user status frameclash", true);
            }
            break;
        }
        case Op::Kind::Poll: break;
    }
    setBusy(Busy::None);
}
