// Copyright (c) 2026 FrameClash contributors. MIT License.
#include "ui.h"

#include "palette.h"
#include "third_party_frameeyeosc/icons.h"
#include "third_party_frameeyeosc/text_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kX0 = 200, kX1 = 1180, kCW = kX1 - kX0;
constexpr int kRows = 9;
constexpr double kRowStep = 54;
constexpr int64_t kArmMs = 4000;

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

std::string ageText(int seconds) {
    seconds = std::max(0, seconds);
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) return std::to_string(seconds / 60) + "m";
    if (seconds < 86400) return std::to_string(seconds / 3600) + "h" + std::to_string(seconds % 3600 / 60) + "m";
    return std::to_string(seconds / 86400) + "d" + std::to_string(seconds % 86400 / 3600) + "h";
}

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
        const bool drop = (cp >= 0x1F000 && cp <= 0x1FAFF) || cp == 0xFE0F || cp == 0x200D ||
                          (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0xE0020 && cp <= 0xE007F);
        if (!drop && !(cp == ' ' && !out.empty() && out.back() == ' ')) out.append(s, i, len);
        i += len;
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return s.empty() || out.empty() ? s : "?";
    return out.substr(first);
}

std::string ellipsizeMono(const Pen& pen, const std::string& text, double size, double width) {
    if (pen.measureMono(text, size) <= width) return text;
    std::string out = text;
    while (!out.empty() && pen.measureMono(out + "…", size) > width) utf8PopBack(out);
    return out + "…";
}

std::vector<std::string> wrapMono(const Pen& pen, const std::string& text, double size, double width, size_t maxLines) {
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < text.size();) {
        const size_t len = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, len);
        if (!lines.back().empty() && pen.measureMono(lines.back() + ch, size) > width) {
            if (lines.size() == maxLines) {
                lines.back() = ellipsizeMono(pen, lines.back() + ch + text.substr(i + len), size, width);
                return lines;
            }
            lines.emplace_back();
        }
        lines.back() += ch;
        i += len;
    }
    return lines;
}

std::string hostPort(const ConnEntry& entry) {
    const std::string port = std::to_string(entry.port);
    return (entry.host.find(':') == std::string::npos ? entry.host : "[" + entry.host + "]") + ":" + port;
}

std::vector<LogLine> filteredLogs(const Snapshot& s, const std::string& filter) {
    std::vector<LogLine> out;
    for (const LogLine& line : s.logLines)
        if (filter == "all" || line.level == filter) out.push_back(line);
    return out;
}

}  // namespace

bool PanelUi::isArmed(const std::string& id) {
    if (armed_ != id) return false;
    if (nowMs() - armedAt_ >= kArmMs) {
        armed_.clear();
        return false;
    }
    return true;
}

bool PanelUi::confirmArmed(const std::string& id) {
    if (isArmed(id)) {
        armed_.clear();
        return true;
    }
    armed_ = id;
    armedAt_ = nowMs();
    return false;
}

bool PanelUi::activateConns(const std::string& id) {
    if (id.rfind("conns:by:", 0) == 0) {
        const std::string by = id.substr(9);
        if (by != "process" && by != "host" && by != "rule" && by != "none") return true;
        connsBy_ = by;
        connsKey_.clear();
        connsOffset_ = 0;
        model_.setDetail(Detail::Conns, connsBy_);
    } else if (id.rfind("conns:grp:", 0) == 0) {
        const size_t i = std::stoul(id.substr(10));
        if (i < last_.connList.groups.size()) {
            connsKey_ = last_.connList.groups[i].key;
            connsOffset_ = 0;
            model_.setDetail(Detail::Conns, connsBy_, connsKey_);
        }
    } else if (id == "conns:back") {
        connsKey_.clear();
        connsOffset_ = 0;
        model_.setDetail(Detail::Conns, connsBy_);
    } else if (id == "conns:up") {
        connsOffset_ = std::max(0, connsOffset_ - kRows);
    } else if (id == "conns:down") {
        connsOffset_ = std::min(maxConnsOffset_, connsOffset_ + kRows);
    } else if (id.rfind("conns:close:", 0) == 0) {
        if (confirmArmed(id)) model_.closeConns("id:" + id.substr(12));
    } else if (id == "conns:closeall") {
        if (confirmArmed(id)) model_.closeConns("all");
    } else if (id == "conns:closegrp") {
        if (confirmArmed(id)) model_.closeConns(connsBy_ + ":" + connsKey_);
    } else {
        return false;
    }
    return true;
}

void PanelUi::drawConnections(const Pen& pen, const Snapshot& s) {
    const ConnList& list = s.connList;
    const bool grouped = connsKey_.empty() && connsBy_ != "none";
    const bool idle = s.busy == Busy::None;
    if (!connsKey_.empty()) {
        button(pen, "conns:back", kX0, 76, 376, 46,
               tr("← ", "← ") + displayText(connsKey_) + "（" + std::to_string(list.matched) + "）", true, cCtl, cText, 17);
    } else {
        const std::string summary = !list.have ? tr("Waiting for connections…", "正在读取连接…") :
            std::to_string(list.total) + tr(" connections · ↑ ", " 个连接 · ↑ ") + humanBytes(list.upTotal) + " ↓ " +
                humanBytes(list.downTotal);
        pen.text(kX0, 106, ellipsize(pen, summary, 17, true, 376, false), 17, list.have ? cText : cMuted, true);
    }
    const struct {
        const char* by;
        std::string label;
        double width;
    } modes[] = {{"process", tr("Process", "程序"), 86}, {"host", tr("Hosts", "主机"), 76},
                 {"rule", tr("Rules", "规则"), 76}, {"none", tr("Details", "明细"), 82}};
    double x = 588;
    for (const auto& mode : modes) {
        const bool selected = connsBy_ == mode.by && connsKey_.empty();
        button(pen, std::string("conns:by:") + mode.by, x, 76, mode.width, 46, mode.label, idle,
               selected ? cAmberTint : cCtl, selected ? cAmber : cText, 15);
        x += mode.width + 6;
    }
    const size_t count = grouped ? list.groups.size() : list.conns.size();
    maxConnsOffset_ = std::max(0, static_cast<int>(count) - kRows);
    connsOffset_ = std::clamp(connsOffset_, 0, maxConnsOffset_);
    button(pen, "conns:up", 938, 76, 40, 46, ICON_TRIANGLE_UP, connsOffset_ > 0, cCtl, cText, 14);
    button(pen, "conns:down", 986, 76, 40, 46, ICON_TRIANGLE_DOWN, connsOffset_ < maxConnsOffset_, cCtl, cText, 14);
    const std::string closeId = connsKey_.empty() ? "conns:closeall" : "conns:closegrp";
    const bool closeArmed = isArmed(closeId);
    button(pen, closeId, 1034, 76, 146, 46,
           closeArmed ? tr("Tap again", "再点一次") : connsKey_.empty() ? tr("Close all", "断开全部") :
                                                                         tr("Close group", "断开这组"),
           idle && list.have && (connsKey_.empty() ? list.total : list.matched) > 0, closeArmed ? cRed : cRedTint, closeArmed ? cOnAmber : cRed, 16);
    if (count == 0) {
        card(pen, kX0, 138, kCW, 486, "");
        const std::string text = !s.status.running ? tr("The proxy is stopped", "代理未运行") :
                                 !list.have         ? tr("Connections are unavailable", "暂时无法读取连接") :
                                                      tr("No connections", "没有连接");
        pen.text(kX0 + (kCW - pen.measure(text, 23, true)) / 2, 370, text, 23, cMuted, true);
        return;
    }
    for (int row = 0; row < kRows && connsOffset_ + row < static_cast<int>(count); ++row) {
        const int i = connsOffset_ + row;
        const double y = 138 + row * kRowStep;
        if (grouped) {
            const ConnGroup& group = list.groups[static_cast<size_t>(i)];
            const std::string id = "conns:grp:" + std::to_string(i);
            const bool hot = hover_ == id || pressed_ == id;
            pen.color(hot ? cCtlHot : cCard);
            pen.roundedRect(kX0, y, kCW, 50, 8);
            cairo_fill(pen.cr);
            pen.text(216, y + 32, ellipsize(pen, displayText(group.key), 19, true, 410, false), 19, cText, true);
            pen.text(700, y + 34, std::to_string(group.count), 25, cAmber, true, true);
            pen.text(736, y + 31,
                     ellipsize(pen, "↑ " + humanBytes(group.up) + "  ↓ " + humanBytes(group.down), 16, false, 270, false),
                     16, cMuted);
            pen.text(1164, y + 31, ageText(group.oldestS), 16, cMuted, false, true);
            addHit(id, kX0, y, kCW, 50, true);
        } else {
            const ConnEntry& entry = list.conns[static_cast<size_t>(i)];
            const std::string id = "conns:close:" + entry.id;
            const bool armed = isArmed(id);
            const bool hot = hover_ == id || pressed_ == id;
            pen.color(armed ? cRedTint : hot ? cCtlHot : cCard);
            pen.roundedRect(kX0, y, kCW, 50, 8);
            cairo_fill(pen.cr);
            pen.text(216, y + 21, ellipsize(pen, hostPort(entry), 18, true, 520, false), 18, armed ? cRed : cText,
                     true);
            const std::string kind = armed ? tr("Tap again to close", "再点一次断开") :
                                            entry.network + " · " + entry.type;
            pen.text(1164, y + 20, ellipsize(pen, kind, 14, false, 390, false), 14, armed ? cRed : cMuted, false, true);
            std::string process = entry.process.empty() ? "?" : entry.process;
            const std::string route = displayText(process + " · " + entry.rule + " → " + entry.group + "/" + entry.chain);
            pen.text(216, y + 43, ellipsize(pen, route, 14, false, 690, false), 14, cMuted);
            const std::string totals = "↑ " + humanBytes(entry.up) + " ↓ " + humanBytes(entry.down) + " · " +
                                       ageText(entry.ageS);
            pen.text(1164, y + 43, totals, 14, cMuted, false, true);
            addHit(id, kX0, y, kCW, 50, idle);
        }
    }
}

bool PanelUi::activateLogs(const std::string& id) {
    if (id.rfind("logs:filter:", 0) == 0) {
        logFilter_ = id.substr(12);
        logOffset_ = 0;
        expandedLog_.clear();
    } else if (id == "logs:prev") {
        logsPrev_ = !logsPrev_;
        logOffset_ = 0;
        expandedLog_.clear();
        model_.setDetail(Detail::Logs, logsPrev_ ? "prev" : "");
    } else if (id == "logs:level") {
        const char* levels[] = {"silent", "error", "warning", "info", "debug"};
        std::string next = "info";
        for (int i = 0; i < 5; ++i)
            if (last_.logLevel == levels[i]) next = levels[(i + 1) % 5];
        model_.setLogLevel(next);
    } else if (id == "logs:clear") {
        if (confirmArmed(id)) {
            logOffset_ = 0;
            expandedLog_.clear();
            model_.clearLog();
        }
    } else if (id == "logs:tail") {
        logOffset_ = 0;
    } else if (id.rfind("logs:line:", 0) == 0) {
        const size_t i = std::stoul(id.substr(10));
        if (i < logView_.size()) {
            const LogLine& line = logView_[i];
            const std::string full = line.marker ? line.msg : line.time + " " + line.level + " " + line.msg;
            expandedLog_ = expandedLog_ == full ? "" : full;
        }
    } else {
        return false;
    }
    return true;
}

void PanelUi::drawLogs(const Pen& pen, const Snapshot& s) {
    const struct {
        const char* level;
        std::string label;
    } filters[] = {{"all", tr("All", "全部")}, {"info", tr("Info", "信息")},
                   {"warning", tr("Warn", "警告")}, {"error", tr("Errors", "错误")}};
    for (int i = 0; i < 4; ++i) {
        const bool selected = logFilter_ == filters[i].level;
        button(pen, std::string("logs:filter:") + filters[i].level, kX0 + i * 70, 76, 64, 46, filters[i].label, true,
               selected ? cAmberTint : cCtl, selected ? cAmber : cText, 14);
    }
    const std::string level = s.logLevel.empty() ? "—" : s.logLevel;
    button(pen, "logs:level", 486, 76, 200, 46, tr("Level: ", "等级：") + level + " " ICON_CHEVRON_RIGHT,
           s.status.running && s.busy == Busy::None, cCtl, cAmber, 16);
    button(pen, "logs:prev", 698, 76, 110, 46, logsPrev_ ? tr("Previous", "上次") : tr("Current", "本次"), true, cCtl,
           cText, 16);
    pen.text(820, 106, tr("reset after restart", "重启后恢复"), 12, cMuted);
    if (logOffset_ > 0)
        button(pen, "logs:tail", 990, 76, 90, 46, tr("↓ Latest", "↓ 最新"), true, cAmberTint, cAmber, 14);
    const bool clearArmed = isArmed("logs:clear");
    button(pen, "logs:clear", 1090, 76, 90, 46, clearArmed ? tr("Sure?", "确定？") : tr("Clear", "清空"),
           s.busy == Busy::None, clearArmed ? cRed : cRedTint, clearArmed ? cOnAmber : cRed, 16);

    // new lines arrive every poll: following (offset 0) takes them, scrolled up keeps the page still
    if (logOffset_ == 0) logView_ = filteredLogs(s, logFilter_);
    const auto& lines = logView_;
    const int n = static_cast<int>(lines.size());
    maxLogOffset_ = std::max(0, n - 21);
    logOffset_ = std::clamp(logOffset_, 0, maxLogOffset_);
    const int end = n - logOffset_;
    const int begin = std::max(0, end - 21);
    if (n == 0) {
        pen.text(kX0 + 30, 180, logsPrev_ ? tr("No previous log", "没有上一次日志") : tr("No log lines", "没有日志"),
                 20, cMuted);
    }
    // silent is the default (daemon render): say how to get lines, the page is otherwise only start markers
    const bool onlyMarkers = std::all_of(lines.begin(), lines.end(), [](const LogLine& l) { return l.marker; });
    if (!logsPrev_ && s.logLevel == "silent" && onlyMarkers && expandedLog_.empty())
        pen.text(kX0 + 30, 600,
                 tr("mihomo logging is off (silent). Tap Level to turn it on until the next restart.",
                    "mihomo 日志已关闭（silent）。点「等级」可临时开启，重启后恢复。"),
                 17, cMuted);
    for (int i = begin; i < end; ++i) {
        const LogLine& line = lines[static_cast<size_t>(i)];
        const int row = i - begin;
        const double y = 138 + row * 22;
        const std::string id = "logs:line:" + std::to_string(i);
        if (hover_ == id || pressed_ == id) {
            pen.color(cCtl);
            cairo_rectangle(pen.cr, kX0, y - 2, kCW, 22);
            cairo_fill(pen.cr);
        }
        if (line.marker) {
            const std::string msg = ellipsize(pen, line.msg, 13, false, 900, false);
            pen.text(216, y + 15, msg, 13, cDim);
            const double after = 216 + pen.measure(msg, 13, false) + 10;
            pen.color(cLine);
            cairo_rectangle(pen.cr, kX0, y + 10, 8, 1);
            cairo_rectangle(pen.cr, after, y + 10, std::max(0.0, kX1 - after), 1);
            cairo_fill(pen.cr);
        } else {
            pen.textMono(204, y + 15, line.time, 15, cMuted);
            const Color levelColor = line.level == "error" ? cRed : line.level == "warning" ? cAmber : cText;
            pen.textMono(282, y + 15, line.level, 15, levelColor);
            pen.textMono(366, y + 15, ellipsizeMono(pen, displayText(line.msg), 15, 794), 15, levelColor);
        }
        addHit(id, kX0, y - 2, kCW, 22, true);
    }
    if (!expandedLog_.empty()) {
        pen.color(cCard);
        pen.roundedRect(kX0, 540, kCW, 84, 8);
        cairo_fill(pen.cr);
        pen.color(cLine);
        cairo_set_line_width(pen.cr, 1);
        pen.roundedRect(kX0 + 0.5, 540.5, kCW - 1, 83, 8);
        cairo_stroke(pen.cr);
        const auto wrapped = wrapMono(pen, expandedLog_, 15, 944, 3);
        for (size_t i = 0; i < wrapped.size(); ++i) pen.textMono(216, 564 + i * 24, wrapped[i], 15, cText);
    }
}
