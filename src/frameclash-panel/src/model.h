// FrameClash panel: the data side. A worker thread talks to frameclashd (so a slow call never stalls the VR
// frame loop) and keeps a snapshot the UI draws from.
// Copyright (c) 2026 FrameClash contributors. MIT License.
#pragma once

#include "api.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class Reach { Unknown, Ok, Unreachable };

enum class Busy {
    None, Starting, Stopping, SwitchingTun, Refreshing, Testing, Selecting, Importing, Activating, Deleting,
    SwitchingMode, CheckingIp, Repairing, Restarting
};

struct Snapshot {
    uint64_t seq = 0;                ///< bumps on every change (the UI redraws when it moves)
    Reach reach = Reach::Unknown;
    std::string reachError;          ///< why frameclashd can't be reached
    DaemonStatus status;
    std::map<std::string, ProxyNode> nodes;
    std::vector<std::string> groups;
    ConnSummary conn;
    double upSpeed = 0, downSpeed = 0;  ///< bytes/s, from the change in conn totals between polls
    ExitIp exitIp;                   ///< last "Check" result (cleared by anything that changes the route)
    std::string exitIpError;
    std::map<std::string, int> testedDelay;  ///< results of this panel's own tests (newest wins over history)
    std::string testingGroup;
    ProfilesInfo profiles;           ///< 0.2.1: saved profiles + YAML files found (from /api/profiles)
    bool haveProfiles = false;       ///< frameclashd answered /api/profiles (false = older daemon)
    uint64_t importSeq = 0;          ///< bumps when an import finishes...
    bool importOk = false;           ///< ...and whether it worked (the UI clears its fields on success)
    Busy busy = Busy::None;
    std::string message;             ///< last action's result (English)
    std::string messageZh;           ///< ...in Chinese
    bool messageIsError = false;
    int64_t messageAtMs = 0;
};

class Model {
public:
    explicit Model(int port);
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    Snapshot snapshot() const;
    /** Ask for a status refresh now. */
    void poll();
    void start();
    void stop();
    void setTun(bool on);
    void setTunDns(bool on);
    void setMode(const std::string& mode);
    /** Ask frameclashd where proxied traffic leaves (on demand only). */
    void checkExitIp();
    /** Stop the proxy and remove TUN leftovers (no root). */
    void repair();
    /** systemctl --user restart frameclash (when frameclashd stopped answering). */
    void restartDaemon();
    void refreshSubscription();
    void select(const std::string& group, const std::string& name);
    void testGroup(const std::string& group);
    void testNode(const std::string& name);
    /** Import a subscription URL (the text is never logged). */
    void importUrl(const std::string& url, const std::string& name);
    /** Import a YAML file by path ("~/..." or relative to home). */
    void importFile(const std::string& path, const std::string& name);
    /** Use (or refresh) a saved profile. */
    void activateProfile(const std::string& id);
    void deleteProfile(const std::string& id);
    /** Poll period: fast while the panel is shown, slow when hidden. */
    void setInterval(int ms);
    /** True when nothing is queued or running. */
    bool idle() const;
    int port() const { return port_; }

private:
    struct Op {
        enum class Kind {
            Poll, Start, Stop, Tun, TunDns, Refresh, Select, TestGroup, TestNode, ImportUrl, ImportFile, Activate, Delete,
            Mode, ExitIp, Repair, RestartDaemon
        } kind;
        std::string a, b;
    };
    int port_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Op> queue_;
    bool running_ = false;  ///< an op is being worked on
    bool quit_ = false;
    int intervalMs_ = 2000;
    Snapshot snap_;
    int64_t lastConnAtMs_ = 0;   ///< when snap_.conn was taken (for the speeds)
    std::thread thread_;

    void push(Op op);
    void loop();
    void doPoll();
    void doAction(const Op& op);
    void setBusy(Busy busy, const std::string& group = "");
    void setMessage(const std::string& en, const std::string& zh, bool error);
    void finishImport(const HttpResult& r, const std::string& okEn, const std::string& okZh, const std::string& failEn,
                      const std::string& failZh);
};

int64_t nowMs();
