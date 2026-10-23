// Copyright (c) 2026 FrameClash contributors. MIT License.
// 0.5.0 layout: a sidebar (Overview / Proxies / Profiles, status, language) and cards, like desktop Clash clients.
// Colors: palette.h. The drawing helpers (Pen, fonts, icons, text layout) come from frameeyeosc.
#include "ui.h"

#include "palette.h"
#include "third_party_frameeyeosc/icons.h"
#include "third_party_frameeyeosc/text_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr double kSideW = 180;
constexpr double kX0 = 200, kX1 = kPanelWidth - 20, kCW = kX1 - kX0;  // content area
constexpr double kTop = 76;                                           // cards start (the page title is above)
constexpr double kFootY = 640, kFootH = 40;                           // message line
constexpr double kGap = 16;
// Proxies page
constexpr double kGroupW = 250;
constexpr double kGroupH = 60, kGroupStep = 68;
constexpr int kGroupsShown = 8;
constexpr double kGridX = kX0 + kGroupW + 16, kGridW = kX1 - kGridX;
constexpr double kGridY = kTop + 60;
constexpr double kCellW = (kGridW - 10) / 2, kCellH = 62, kCellStep = 70;
constexpr int kRowsShown = 7;
constexpr int64_t kRepairArmMs = 4000;

std::string humanBytes(double n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (n >= 1024 && u < 4) {
        n /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", n, units[u]);
    return buf;
}

Color delayColor(int ms) {
    if (ms <= 0) return cRed;
    if (ms < 300) return cGreen;
    if (ms < 800) return cAmber;
    return cRed;
}

/** Subscriptions often prefix node names with flag emoji (regional indicators) and other pictographs. The CJK font has
 * no glyphs for them (they'd draw as boxes), so they are left out of what is shown; the real name is still used for
 * selecting. */
std::string displayText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
        if (i + len > s.size()) len = 1;
        uint32_t cp = c;
        if (len == 2) cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f);
        if (len == 3) cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
        if (len == 4) cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f);
        const bool drop = (cp >= 0x1F000 && cp <= 0x1FAFF) || cp == 0xFE0F || cp == 0x200D || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0xE0020 && cp <= 0xE007F);
        if (!drop && !(cp == ' ' && !out.empty() && out.back() == ' ')) out.append(s, i, len);
        i += len;
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return s.empty() || out.empty() ? s : "?";
    return out.substr(b);
}

bool selectable(const ProxyNode& group) {
    return group.type == "Selector";
}

std::string delayText(int ms, bool zh) {
    return ms < 0 ? "—" : ms == 0 ? (zh ? "超时" : "timeout") : std::to_string(ms) + " ms";
}

}  // namespace

PanelUi::PanelUi(Model& model, const FontSet& fonts, std::function<void()> openSettings, KeyboardHooks keyboard)
    : model_(model), fonts_(fonts), openSettings_(std::move(openSettings)), kbd_(std::move(keyboard)) {}

void PanelUi::addHit(const std::string& id, double x, double y, double w, double h, bool enabled) {
    hits_.push_back({id, x, y, w, h, enabled});
}

const Hit* PanelUi::hitAt(double x, double y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return &*it;
    return nullptr;
}

void PanelUi::button(const Pen& pen, const std::string& id, double x, double y, double w, double h,
                     const std::string& label, bool enabled, Color fill, Color text, double size) {
    const bool hot = enabled && (hover_ == id || pressed_ == id);
    Color f = enabled ? fill : cCtl;
    if (hot) f = blendColor(hexColor(0xffffff), f, pressed_ == id ? 0.18 : 0.08);
    pen.color(f);
    pen.roundedRect(x, y, w, h, 8);
    cairo_fill(pen.cr);
    const std::string fit = ellipsize(pen, label, size, true, w - 20, false);
    const double tw = pen.measure(fit, size, true);
    pen.text(x + (w - tw) / 2, y + h / 2 + size * 0.36, fit, size, enabled ? text : cDim, true);
    addHit(id, x, y, w, h, enabled);
}

void PanelUi::card(const Pen& pen, double x, double y, double w, double h, const std::string& label,
                   const std::string& hoverId) {
    const bool hot = !hoverId.empty() && (hover_ == hoverId || pressed_ == hoverId);
    pen.color(hot ? cCtl : cCard);
    pen.roundedRect(x, y, w, h, 12);
    cairo_fill(pen.cr);
    pen.color(cLine);
    cairo_set_line_width(pen.cr, 1);
    pen.roundedRect(x + 0.5, y + 0.5, w - 1, h - 1, 12);
    cairo_stroke(pen.cr);
    if (!label.empty()) pen.text(x + 20, y + 34, label, 16, cMuted, true);
}

void PanelUi::toggle(const Pen& pen, double x, double y, bool on, bool enabled) {
    pen.color(!enabled ? cCtl : on ? cAmber : cCtlHot);
    pen.roundedRect(x, y, 60, 32, 16);
    cairo_fill(pen.cr);
    cairo_arc(pen.cr, on ? x + 44 : x + 16, y + 16, 11, 0, 2 * M_PI);
    pen.color(!enabled ? cDim : on ? cOnAmber : cMuted);
    cairo_fill(pen.cr);
}

int PanelUi::delayOf(const Snapshot& s, const std::string& name) const {
    auto tested = s.testedDelay.find(name);
    if (tested != s.testedDelay.end()) return tested->second;
    auto node = s.nodes.find(name);
    return node == s.nodes.end() ? -1 : node->second.delay;
}

std::string PanelUi::busyLabel(Busy busy) const {
    switch (busy) {
        case Busy::Starting: return tr("Starting…", "正在启动…");
        case Busy::Stopping: return tr("Stopping…", "正在停止…");
        case Busy::SwitchingTun: return tr("Switching TUN…", "正在切换 TUN…");
        case Busy::Refreshing: return tr("Refreshing subscription…", "正在更新订阅…");
        case Busy::Testing: return tr("Testing latency…", "正在测速…");
        case Busy::Selecting: return tr("Switching node…", "正在切换节点…");
        case Busy::Importing: return tr("Importing…", "正在导入…");
        case Busy::Activating: return tr("Loading profile…", "正在载入配置…");
        case Busy::Deleting: return tr("Removing…", "正在移除…");
        case Busy::SwitchingMode: return tr("Switching mode…", "正在切换模式…");
        case Busy::CheckingIp: return tr("Checking…", "正在检测…");
        case Busy::Repairing: return tr("Repairing the network…", "正在修复网络…");
        case Busy::Restarting: return tr("Restarting frameclashd…", "正在重启后台服务…");
        case Busy::None: break;
    }
    return "";
}

void PanelUi::draw(cairo_t* cr, const Snapshot& s) {
    last_ = s;
    hits_.clear();
    const Pen pen {cr, &fonts_};
    pen.color(cBg);
    cairo_paint(cr);
    trackImport(s);
    drawSidebar(pen, s);
    // page title, and what is running right now
    const std::string title = view_ == View::Overview ? tr("Overview", "概览")
                              : view_ == View::Proxies ? tr("Proxies", "代理")
                                                       : tr("Profiles", "配置");
    pen.text(kX0, 52, title, 28, cText, true);
    const std::string busy = busyLabel(s.busy);
    if (!busy.empty()) pen.text(kX1, 50, busy, 17, cAmber, true, true);
    if (s.reach == Reach::Unreachable && view_ != View::Profiles) {
        drawRescue(pen, s);
    } else if (view_ == View::Overview) {
        drawOverview(pen, s);
    } else if (view_ == View::Proxies) {
        drawProxies(pen, s);
    } else {
        drawProfilesView(pen, s);
    }
    drawMessage(pen, s, kX0, kFootY, kCW, kFootH);
}

void PanelUi::drawSidebar(const Pen& pen, const Snapshot& s) {
    pen.color(cSide);
    cairo_rectangle(pen.cr, 0, 0, kSideW, kPanelHeight);
    cairo_fill(pen.cr);
    pen.color(cLine);
    cairo_rectangle(pen.cr, kSideW - 1, 0, 1, kPanelHeight);
    cairo_fill(pen.cr);
    pen.color(cAmber);
    pen.roundedRect(22, 33, 14, 14, 3);
    cairo_fill(pen.cr);
    pen.text(44, 48, "FrameClash", 21, cText, true);

    const struct {
        View v;
        const char* id;
        std::string label;
    } items[] = {{View::Overview, "view:overview", tr("Overview", "概览")},
                 {View::Proxies, "view:proxies", tr("Proxies", "代理")},
                 {View::Profiles, "view:profiles", tr("Profiles", "配置")}};
    double y = 92;
    for (const auto& it : items) {
        const bool sel = view_ == it.v;
        const bool hot = hover_ == it.id || pressed_ == it.id;
        if (sel || hot) {
            pen.color(sel ? cAmberTint : cCtl);
            pen.roundedRect(12, y, kSideW - 24, 50, 8);
            cairo_fill(pen.cr);
        }
        if (sel) {
            pen.color(cAmber);
            pen.roundedRect(12, y + 12, 4, 26, 2);
            cairo_fill(pen.cr);
        }
        pen.text(32, y + 33, it.label, 20, sel ? cAmber : cText, sel);
        addHit(it.id, 12, y, kSideW - 24, 50, true);
        y += 58;
    }

    // proxy state, language, version
    Color c = cMuted;
    std::string state;
    if (s.reach == Reach::Unreachable) state = tr("Service down", "服务无响应"), c = cRed;
    else if (s.reach == Reach::Unknown) state = tr("Connecting…", "连接中…");
    else if (s.status.running) state = tr("Running", "运行中"), c = cGreen;
    else state = tr("Stopped", "已停止");
    cairo_arc(pen.cr, 28, 574, 6, 0, 2 * M_PI);
    pen.color(c);
    cairo_fill(pen.cr);
    pen.text(44, 580, state, 18, s.reach == Reach::Ok && s.status.running ? cText : c, true);
    button(pen, "lang", 12, 600, kSideW - 24, 42, zh_ ? "English" : "中文", true, cCtl, cText, 17);
    if (!s.status.version.empty()) pen.text(22, 674, "v" + s.status.version, 14, cMuted);
}

void PanelUi::drawOverview(const Pen& pen, const Snapshot& s) {
    const bool reachable = s.reach == Reach::Ok;
    const bool running = reachable && s.status.running;
    const bool idle = reachable && s.busy == Busy::None;
    const double halfW = (kCW - kGap) / 2, x2 = kX0 + halfW + kGap;

    // ---- row 1: proxy on/off | speed ----
    double y = kTop;
    card(pen, kX0, y, halfW, 170, tr("PROXY", "代理"));
    {
        const double x = kX0 + 20;
        cairo_arc(pen.cr, x + 8, y + 74, 8, 0, 2 * M_PI);
        pen.color(running ? cGreen : cDim);
        cairo_fill(pen.cr);
        pen.text(x + 26, y + 84, s.reach == Reach::Unknown ? tr("Connecting…", "连接中…")
                                 : running                  ? tr("Running", "运行中")
                                                            : tr("Stopped", "已停止"),
                 30, cText, true);
        std::string profile = s.status.source.empty() ? tr("No profile yet", "尚未导入配置") : "";
        for (const ProfileEntry& p : s.profiles.profiles)
            if (p.active) profile = p.name;
        if (profile.empty()) profile = s.status.source == "url" ? tr("Subscription", "订阅") : tr("Local YAML", "本地 YAML");
        pen.text(x, y + 118, ellipsize(pen, tr("Profile: ", "配置：") + displayText(profile), 16, false, halfW - 230, false),
                 16, s.status.source.empty() ? cAmber : cMuted);
        pen.text(x, y + 148, running ? s.status.proxy + tr("  (HTTP + SOCKS5)", "（HTTP + SOCKS5）") : "", 15, cMuted);
        const double bx = kX0 + halfW - 20 - 180;
        if (running)
            button(pen, "toggle", bx, y + 54, 180, 56, tr("Stop", "停止"), idle, cCtl, cRed, 21);
        else if (s.status.source.empty())
            button(pen, "view:profiles", bx, y + 54, 180, 56, tr("Import", "导入配置") + " " ICON_CHEVRON_RIGHT, reachable,
                   cAmber, cOnAmber, 20);
        else
            button(pen, "toggle", bx, y + 54, 180, 56, ICON_PLAY " " + tr("Start", "启动"), idle, cAmber, cOnAmber, 21);
    }
    card(pen, x2, y, halfW, 170, tr("SPEED", "网络速度"));
    {
        const double colW = (halfW - 40) / 2;
        const struct {
            const char* arrow;
            std::string label;
            double v;
        } cols[] = {{"↑", tr("Upload", "上传"), s.upSpeed}, {"↓", tr("Download", "下载"), s.downSpeed}};
        for (int i = 0; i < 2; ++i) {
            const double x = x2 + 20 + i * colW;
            pen.text(x, y + 88, std::string(cols[i].arrow) + " " + (running ? humanBytes(cols[i].v) + "/s" : "—"), 28,
                     running ? cText : cDim, true);
            pen.text(x, y + 114, cols[i].label, 15, cMuted);
        }
        if (running && s.conn.have)
            pen.text(x2 + 20, y + 148,
                     tr("Total ", "累计 ") + "↑ " + humanBytes(static_cast<double>(s.conn.upload)) + "  ↓ " +
                         humanBytes(static_cast<double>(s.conn.download)) + "  ·  " +
                         std::to_string(s.conn.connections) + tr(" connections", " 个连接"),
                     15, cMuted);
    }

    // ---- row 2: TUN | DNS | mode ----
    y += 170 + kGap;
    const double thirdW = (kCW - 2 * kGap) / 3;
    auto desc = [&](double x, const std::string& text, Color c) {
        double ly = y + 92;
        for (const std::string& line : wrapText(pen, text, 15, false, thirdW - 40, 2)) {
            pen.text(x + 20, ly, line, 15, c);
            ly += 22;
        }
    };
    {
        const double x = kX0;
        card(pen, x, y, thirdW, 140, tr("TUN", "虚拟网卡 TUN"), "tun");
        toggle(pen, x + thirdW - 80, y + 16, s.status.tun, idle);
        std::string d;
        Color c = cMuted;
        if (!s.status.tunReady.empty() && !s.status.tun) {
            d = tr("Not set up: run tun-setup.sh install once (sudo)", "未安装：先运行一次 tun-setup.sh install（sudo）");
            c = cAmber;
        } else if (s.status.tun) {
            d = tr("All wlan0 traffic via the proxy; streaming Wi-Fi and LAN stay direct",
                   "wlan0 全部流量走代理；串流网卡与局域网直连");
        } else {
            d = tr("Off: only apps using the system proxy", "关闭：仅使用系统代理的应用走代理");
        }
        desc(x, d, c);
        addHit("tun", x, y, thirdW, 140, idle);
    }
    {
        const double x = kX0 + thirdW + kGap;
        const bool tunUp = running && s.status.tun;
        const bool on = tunUp ? s.status.tunDns == "on" : s.status.selfDns;
        card(pen, x, y, thirdW, 140, tr("DNS", "DNS 接管"), "tundns");
        toggle(pen, x + thirdW - 80, y + 16, on, idle);
        std::string d;
        Color c = cMuted;
        if (!tunUp) d = tr("Applies while TUN runs: system DNS through the tun", "TUN 运行时生效：系统 DNS 走 TUN");
        else if (s.status.tunDns == "on") d = tr("System DNS goes through the tun", "系统 DNS 走 TUN");
        else if (s.status.tunDns == "off") d = tr("System DNS as the network set it", "使用网络自带的 DNS");
        else if (s.status.tunDns == "no-permission")
            d = tr("No permission: run tun-setup.sh install", "无权限：需运行 tun-setup.sh install"), c = cAmber;
        else if (s.status.tunDns.empty()) d = tr("Setting up…", "正在设置…");
        else d = s.status.tunDns, c = cRed;
        desc(x, d, c);
        addHit("tundns", x, y, thirdW, 140, idle);
    }
    {
        const double x = kX0 + 2 * (thirdW + kGap);
        card(pen, x, y, thirdW, 140, tr("MODE", "出站模式"));
        const std::string mode = s.status.mode.empty() ? "rule" : s.status.mode;
        const struct {
            const char* id;
            std::string label;
        } modes[] = {{"rule", tr("Rule", "规则")}, {"global", tr("Global", "全局")}, {"direct", tr("Direct", "直连")}};
        const double segW = (thirdW - 40 - 8) / 3;
        for (int i = 0; i < 3; ++i) {
            const bool sel = mode == modes[i].id;
            button(pen, std::string("mode:") + modes[i].id, x + 20 + i * (segW + 4), y + 60, segW, 52, modes[i].label,
                   idle && !s.status.mode.empty(), sel ? cAmber : cCtl, sel ? cOnAmber : cText, 18);
        }
    }

    // ---- row 3: current node | network check ----
    y += 140 + kGap;
    const double h3 = kFootY - kGap - y;
    {
        const std::string group = running && !s.groups.empty() ? s.groups.front() : "";
        card(pen, kX0, y, halfW, h3, tr("NODE", "当前节点"));
        const double x = kX0 + 20;
        if (!group.empty()) {
            pen.text(kX0 + halfW - 20, y + 34, ellipsize(pen, displayText(group), 15, false, halfW - 160, false), 15,
                     cMuted, false, true);
            const std::string leaf = resolveLeaf(s.nodes, group);
            const int ms = delayOf(s, leaf);
            const std::string d = delayText(ms, zh_);
            const double dw = pen.measure(d, 17, true);
            pen.text(x, y + 80, ellipsize(pen, displayText(leaf), 24, true, halfW - dw - 80, false), 24, cText, true);
            pen.text(kX0 + halfW - 20, y + 80, d, 17, ms < 0 ? cMuted : delayColor(ms), true, true);
            auto it = s.nodes.find(leaf);
            pen.text(x, y + 108, it == s.nodes.end() ? "" : it->second.type, 15, cMuted);
            const bool testing = s.busy == Busy::Testing;
            button(pen, "nodetest:" + leaf, x, y + h3 - 68, 150, 48, testing ? tr("Testing…", "测速中…") : tr("Test", "测速"),
                   idle, cCtl, cAmber, 18);
        } else {
            pen.text(x, y + 80, running ? tr("No proxy groups", "没有代理组") : tr("Start the proxy to see nodes", "启动代理后显示节点"),
                     18, cMuted);
        }
        button(pen, "view:proxies", x + 160, y + h3 - 68, halfW - 200, 48,
               tr("Choose node", "切换节点") + " " ICON_CHEVRON_RIGHT, true, cCtl, cText, 18);
    }
    {
        card(pen, x2, y, halfW, h3, tr("NETWORK", "网络检测"));
        const double x = x2 + 20;
        if (!s.exitIp.ip.empty()) {
            pen.textMono(x, y + 80, s.exitIp.ip, 24, cText);
            std::string where = s.exitIp.country;
            if (!s.exitIp.city.empty()) where += " · " + s.exitIp.city;
            if (!s.exitIp.org.empty()) where += " · " + s.exitIp.org;
            pen.text(x, y + 108, ellipsize(pen, where, 15, false, halfW - 40, false), 15, cMuted);
        } else if (!s.exitIpError.empty()) {
            pen.text(x, y + 80, tr("Check failed", "检测失败"), 22, cRed, true);
            pen.text(x, y + 108, ellipsize(pen, s.exitIpError, 15, false, halfW - 40, false), 15, cMuted);
        } else {
            pen.text(x, y + 80, tr("Exit IP: —", "出口 IP：—"), 22, cMuted, true);
            pen.text(x, y + 108, tr("Asks ipinfo.io through the proxy", "通过代理访问 ipinfo.io"), 15, cMuted);
        }
        const double bw = (halfW - 50) / 2;
        button(pen, "exitip", x, y + h3 - 68, bw, 48,
               s.busy == Busy::CheckingIp ? tr("Checking…", "检测中…") : tr("Check exit IP", "检测出口 IP"), idle && running,
               cCtl, cText, 18);
        const bool armed = repairArmedAt_ != 0 && nowMs() - repairArmedAt_ < kRepairArmMs;
        if (!armed) repairArmedAt_ = 0;
        button(pen, "repair", x + bw + 10, y + h3 - 68, bw, 48,
               armed ? tr("Tap again to repair", "再点一次确认") : tr("Repair network", "修复网络"), idle,
               armed ? cRed : cRedTint, armed ? cOnAmber : cRed, 18);
    }
}

void PanelUi::drawProxies(const Pen& pen, const Snapshot& s) {
    const bool running = s.reach == Reach::Ok && s.status.running;
    if (!running || s.groups.empty()) {
        maxRowOffset_ = maxGroupOffset_ = 0;
        card(pen, kX0, kTop, kCW, kFootY - kGap - kTop, "");
        std::string title, hint;
        if (s.reach == Reach::Unknown) {
            title = tr("Connecting…", "连接中…");
        } else if (!running) {
            title = tr("The proxy is stopped", "代理未运行");
            hint = s.status.source.empty() ? tr("Import a subscription or config file in Profiles first.",
                                                "请先在「配置」页导入订阅或配置文件。")
                                           : tr("Start it on the Overview page to load the nodes.",
                                                "在「概览」页启动代理后加载节点。");
        } else {
            title = tr("No proxy groups", "没有代理组");
            hint = tr("The profile has no selectable groups.", "当前配置中没有可选的代理组。");
        }
        const double cy = (kTop + kFootY) / 2;
        pen.text(kX0 + (kCW - pen.measure(title, 26, true)) / 2, cy - 6, title, 26, cText, true);
        pen.text(kX0 + (kCW - pen.measure(hint, 18)) / 2, cy + 30, hint, 18, cMuted);
        return;
    }
    if (std::find(s.groups.begin(), s.groups.end(), group_) == s.groups.end()) {
        group_ = s.groups.front();
        rowOffset_ = 0;
    }
    const bool idle = s.busy == Busy::None;

    // ---- group list (paged with ▲ ▼ when it doesn't fit) ----
    const int ng = static_cast<int>(s.groups.size());
    const bool pagedGroups = ng > kGroupsShown;
    const int shown = pagedGroups ? kGroupsShown - 1 : ng;
    maxGroupOffset_ = std::max(0, ng - shown);
    groupOffset_ = std::clamp(groupOffset_, 0, maxGroupOffset_);
    for (int r = 0; r < shown && groupOffset_ + r < ng; ++r) {
        const int i = groupOffset_ + r;
        const std::string& g = s.groups[static_cast<size_t>(i)];
        const double gy = kTop + r * kGroupStep;
        const bool sel = g == group_;
        const std::string id = "grp:" + std::to_string(i);
        const bool hot = hover_ == id || pressed_ == id;
        pen.color(sel ? cAmberTint : hot ? cCtlHot : cCard);
        pen.roundedRect(kX0, gy, kGroupW, kGroupH, 10);
        cairo_fill(pen.cr);
        if (sel) {
            pen.color(cAmber);
            pen.roundedRect(kX0, gy + 14, 4, kGroupH - 28, 2);
            cairo_fill(pen.cr);
        }
        pen.text(kX0 + 18, gy + 26, ellipsize(pen, displayText(g), 18, true, kGroupW - 36, false), 18,
                 sel ? cAmber : cText, true);
        pen.text(kX0 + 18, gy + 48, ellipsize(pen, displayText(resolveLeaf(s.nodes, g)), 14, false, kGroupW - 36, false),
                 14, cMuted);
        addHit(id, kX0, gy, kGroupW, kGroupH, true);
    }
    if (pagedGroups) {
        const double py = kTop + shown * kGroupStep, bw = (kGroupW - 8) / 2;
        button(pen, "grpup", kX0, py, bw, 48, ICON_TRIANGLE_UP, groupOffset_ > 0, cCtl, cText, 16);
        button(pen, "grpdown", kX0 + bw + 8, py, bw, 48, ICON_TRIANGLE_DOWN, groupOffset_ < maxGroupOffset_, cCtl, cText,
               16);
    }

    // ---- header: group info, paging, sort, test ----
    const ProxyNode& g = s.nodes.at(group_);
    const bool canSelect = selectable(g);
    const int count = static_cast<int>(g.all.size());
    const int rows = (count + 1) / 2;
    maxRowOffset_ = std::max(0, rows - kRowsShown);
    rowOffset_ = std::clamp(rowOffset_, 0, maxRowOffset_);
    const bool testingThis = s.busy == Busy::Testing && s.testingGroup == group_;
    double bx = kX1 - 120;
    button(pen, "test", bx, kTop, 120, 46, testingThis ? tr("Testing…", "测速中…") : tr("Test all", "全部测速"), idle,
           cAmber, cOnAmber, 17);
    bx -= 8 + 130;
    button(pen, "sort", bx, kTop, 130, 46, sortByDelay_ ? tr("By latency", "按延迟") : tr("As listed", "默认排序"), true,
           sortByDelay_ ? cAmberTint : cCtl, sortByDelay_ ? cAmber : cText, 17);
    if (maxRowOffset_ > 0) {
        bx -= 12 + 46;
        button(pen, "down", bx, kTop, 46, 46, ICON_TRIANGLE_DOWN, rowOffset_ < maxRowOffset_, cCtl, cText, 15);
        bx -= 6 + 46;
        button(pen, "up", bx, kTop, 46, 46, ICON_TRIANGLE_UP, rowOffset_ > 0, cCtl, cText, 15);
    }
    std::string info = g.type + " · " + std::to_string(count) + tr(" nodes", " 个节点");
    if (!canSelect) info += tr(" · auto", " · 自动选择");
    pen.text(kGridX, kTop + 30, ellipsize(pen, info, 16, false, bx - kGridX - 12, false), 16, cMuted);

    // ---- node grid ----
    std::vector<std::string> order = g.all;
    if (sortByDelay_) {
        auto rank = [&](const std::string& n) {  // tested ones by delay, then untested, then timeouts
            const int ms = delayOf(s, n);
            return ms > 0 ? ms : ms < 0 ? 1000000 : 2000000;
        };
        std::stable_sort(order.begin(), order.end(),
                         [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    }
    for (int r = 0; r < kRowsShown; ++r) {
        for (int c = 0; c < 2; ++c) {
            const int idx = (rowOffset_ + r) * 2 + c;
            if (idx >= count) break;
            const std::string& name = order[static_cast<size_t>(idx)];
            const double cx = kGridX + c * (kCellW + 10);
            const double cy = kGridY + r * kCellStep;
            const bool current = name == g.now;
            const std::string id = "node:" + name;
            const bool hot = canSelect && idle && (hover_ == id || pressed_ == id);
            pen.color(current ? cAmberTint : hot ? cCtlHot : cCard);
            pen.roundedRect(cx, cy, kCellW, kCellH, 10);
            cairo_fill(pen.cr);
            pen.color(current ? cAmber : cLine);
            cairo_set_line_width(pen.cr, current ? 2 : 1);
            pen.roundedRect(cx + 1, cy + 1, kCellW - 2, kCellH - 2, 9);
            cairo_stroke(pen.cr);
            auto it = s.nodes.find(name);
            std::string type = it == s.nodes.end() ? "" : it->second.type;
            if (it != s.nodes.end() && it->second.isGroup()) type += " " ICON_ARROW_RIGHT " " + it->second.now;
            const int ms = delayOf(s, name);
            const std::string delay = delayText(ms, zh_);
            const double dw = pen.measure(delay, 16, true);
            pen.text(cx + 16, cy + 27, ellipsize(pen, displayText(name), 18, true, kCellW - dw - 44, false), 18,
                     current ? cAmber : cText, true);
            pen.text(cx + 16, cy + 50, ellipsize(pen, displayText(type), 14, false, kCellW - dw - 44, false), 14, cMuted);
            pen.text(cx + kCellW - 16, cy + 38, delay, 16, ms < 0 ? cMuted : delayColor(ms), true, true);
            if (canSelect) addHit(id, cx, cy, kCellW, kCellH, idle && !current);
        }
    }
}

void PanelUi::drawRescue(const Pen& pen, const Snapshot& s) {
    card(pen, kX0, kTop, kCW, 300, "");
    const double x = kX0 + 28;
    pen.text(x, kTop + 62, tr("frameclashd is not answering", "后台服务 frameclashd 无响应"), 28, cText, true);
    double y = kTop + 100;
    for (const std::string& line :
         wrapText(pen, tr("The panel can't reach 127.0.0.1:", "面板无法连接 127.0.0.1:") + std::to_string(model_.port()) +
                           " (" + s.reachError + ")",
                  17, false, kCW - 56, 2)) {
        pen.text(x, y, line, 17, cMuted);
        y += 26;
    }
    button(pen, "restartd", x, kTop + 160, 300, 58, tr("Restart service", "重启后台服务"), s.busy == Busy::None, cAmber,
           cOnAmber, 21);
    pen.text(x + 320, kTop + 186, tr("No root needed. The proxy comes back on if it was on.", "无需 root。之前开着的代理会自动恢复。"),
             16, cMuted);
    pen.text(x, kTop + 258, tr("Over SSH:", "SSH："), 16, cMuted);
    pen.textMono(x + 80, kTop + 258, "systemctl --user restart frameclash", 16, cAmber);
}

void PanelUi::drawThumbnail(cairo_t* cr, const Snapshot& s) {
    const Pen pen {cr, &fonts_};
    pen.color(cCard);
    cairo_paint(cr);
    const double tw = pen.measure("FC", 118, true);
    pen.text((kThumbnailSize - tw) / 2, 168, "FC", 118, cText, true);
    const Color c = s.reach != Reach::Ok ? cRed : s.status.running ? cGreen : cDim;
    cairo_arc(cr, 214, 42, 24, 0, 2 * M_PI);
    pen.color(c);
    cairo_fill(cr);
    pen.color(cAmber);
    cairo_rectangle(cr, 48, 200, kThumbnailSize - 96, 10);
    cairo_fill(cr);
}

bool PanelUi::pointerMove(double x, double y) {
    const Hit* h = hitAt(x, y);
    const std::string id = h != nullptr && h->enabled ? h->id : "";
    if (id == hover_) return false;
    hover_ = id;
    return true;
}

bool PanelUi::pointerDown(double x, double y) {
    const Hit* h = hitAt(x, y);
    pressed_ = h != nullptr && h->enabled ? h->id : "";
    hover_ = pressed_;
    return true;
}

bool PanelUi::pointerUp(double x, double y) {
    const Hit* h = hitAt(x, y);
    const std::string id = h != nullptr && h->enabled ? h->id : "";
    const std::string was = pressed_;
    pressed_.clear();
    if (!id.empty() && id == was) activate(id);  // only a press and release on the same target counts
    return true;
}

bool PanelUi::pointerLeave() {
    const bool changed = !hover_.empty() || !pressed_.empty();
    hover_.clear();
    pressed_.clear();
    return changed;
}

bool PanelUi::scroll(double dy) {
    // the list under the pointer scrolls: profiles, the group list, or the node grid
    int* offset = &rowOffset_;
    int maxOff = maxRowOffset_;
    double step = kCellStep;
    if (view_ == View::Profiles) {
        offset = &profOffset_;
        maxOff = std::max(0, static_cast<int>(last_.profiles.profiles.size()) - 5);
        step = 76;
    } else if (view_ != View::Proxies) {
        return false;
    } else if (hover_.rfind("grp", 0) == 0) {
        offset = &groupOffset_;
        maxOff = maxGroupOffset_;
        step = kGroupStep;
    }
    scrollAccum_ += dy;
    bool changed = false;
    while (scrollAccum_ >= step) {
        scrollAccum_ -= step;
        if (*offset < maxOff) ++*offset, changed = true;
    }
    while (scrollAccum_ <= -step) {
        scrollAccum_ += step;
        if (*offset > 0) --*offset, changed = true;
    }
    return changed;
}

bool PanelUi::press(const std::string& id) {
    for (const Hit& h : hits_)
        if (h.id == id && h.enabled) {
            activate(id);
            return true;
        }
    return false;
}

bool PanelUi::showGroup(const std::string& name) {
    for (size_t i = 0; i < last_.groups.size(); ++i)
        if (last_.groups[i] == name) {
            view_ = View::Proxies;
            group_ = name;
            rowOffset_ = 0;
            const int gi = static_cast<int>(i);
            if (gi < groupOffset_ || gi >= groupOffset_ + kGroupsShown - 1) groupOffset_ = gi;
            return true;
        }
    return false;
}

void PanelUi::activate(const std::string& id) {
    if (id == "view:overview") {
        view_ = View::Overview;
    } else if (id == "view:proxies") {
        view_ = View::Proxies;
    } else if (id == "view:profiles") {
        view_ = View::Profiles;
    } else if (activateProfiles(id)) {
    } else if (id == "toggle") {
        if (last_.status.running) model_.stop();
        else model_.start();
    } else if (id == "tun") {
        model_.setTun(!last_.status.tun);
    } else if (id == "tundns") {
        const bool tunUp = last_.status.running && last_.status.tun;
        model_.setTunDns(!(tunUp ? last_.status.tunDns == "on" : last_.status.selfDns));
    } else if (id.rfind("mode:", 0) == 0) {
        model_.setMode(id.substr(5));
    } else if (id == "exitip") {
        model_.checkExitIp();
    } else if (id == "repair") {
        if (repairArmedAt_ != 0 && nowMs() - repairArmedAt_ < kRepairArmMs) {
            repairArmedAt_ = 0;
            model_.repair();
        } else {
            repairArmedAt_ = nowMs();
        }
    } else if (id == "restartd") {
        model_.restartDaemon();
    } else if (id.rfind("nodetest:", 0) == 0) {
        model_.testNode(id.substr(9));
    } else if (id == "settings") {
        if (openSettings_) openSettings_();
    } else if (id == "lang") {
        zh_ = !zh_;
    } else if (id == "test") {
        model_.testGroup(group_);
    } else if (id == "sort") {
        sortByDelay_ = !sortByDelay_;
        rowOffset_ = 0;
    } else if (id == "grpup") {
        groupOffset_ = std::max(0, groupOffset_ - (kGroupsShown - 1));
    } else if (id == "grpdown") {
        groupOffset_ = std::min(maxGroupOffset_, groupOffset_ + (kGroupsShown - 1));
    } else if (id == "up") {
        rowOffset_ = std::max(0, rowOffset_ - kRowsShown);
    } else if (id == "down") {
        rowOffset_ = std::min(maxRowOffset_, rowOffset_ + kRowsShown);
    } else if (id.rfind("grp:", 0) == 0) {
        const size_t i = std::stoul(id.substr(4));
        if (i < last_.groups.size()) {
            group_ = last_.groups[i];
            rowOffset_ = 0;
        }
    } else if (id.rfind("node:", 0) == 0) {
        model_.select(group_, id.substr(5));
    }
}
