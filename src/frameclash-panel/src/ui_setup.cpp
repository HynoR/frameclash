// Copyright (c) 2026 FrameClash contributors. MIT License.
#include "ui.h"

#include "palette.h"
#include "third_party_frameeyeosc/icons.h"
#include "third_party_frameeyeosc/text_layout.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kX0 = 200, kX1 = 1180, kCW = kX1 - kX0;

std::string setupScript(const TunSetup& setup) {
    return setup.script.empty() ? "~/.local/lib/frameclash/tun-setup.sh" : setup.script;
}

std::string setupCommand(const TunSetup& setup, const std::string& verb) {
    return setupScript(setup) + " " + (verb == "no-self-dns" ? "install --no-self-dns" : verb);
}

}  // namespace

bool PanelUi::activateSetup(const std::string& id) {
    if (id == "setup:check") {
        model_.checkSetup();
        return true;
    }
    if (id.rfind("copy:", 0) != 0) return false;
    const std::string verb = id.substr(5);
    if (verb != "install" && verb != "cleanup" && verb != "remove" && verb != "status" && verb != "no-self-dns")
        return false;
    const std::string command = setupCommand(last_.setup, verb);
    const std::string error = kbd_.copy ? kbd_.copy(command) : "clipboard hook unavailable";
    if (error.empty()) {
        note("Copied. Paste it into the Frame terminal.", "已复制，到终端粘贴", false);
    } else {
        note("Could not copy (" + error + "); type the command shown above.",
             "无法复制（" + error + "），请照上面输入", true);
    }
    return true;
}

void PanelUi::drawSetup(const Pen& pen, const Snapshot& s) {
    const TunSetup& setup = s.setup;
    const bool idle = s.reach == Reach::Ok && s.busy == Busy::None;
    card(pen, kX0, 76, kCW, 170, tr("STATUS", "状态"));
    button(pen, "setup:check", 1000, 92, 160, 36, tr("Check again", "重新检查"), s.busy == Busy::None, cCtl, cText, 16);
    if (s.reach == Reach::Unreachable) {
        pen.text(220, 150, tr("frameclashd is not answering", "后台服务无响应"), 22, cRed, true);
        pen.text(220, 190, tr("The commands below are still available. Check again after the service returns.",
                              "以下命令仍可使用；服务恢复后请按「重新检查」。"),
                 16, cMuted);
    } else {
        auto statusRow = [&](double y, const std::string& label, const std::string& value, int state, Color color) {
            const std::string mark = state > 0 ? ICON_CHECK : state < 0 ? ICON_CROSS : "—";
            pen.text(222, y, mark, 17, color, true);
            pen.text(252, y, label, 17, cMuted);
            pen.text(500, y, ellipsize(pen, value, 17, false, 650, false), 17, color);
        };
        if (!setup.have) {
            statusRow(148, "TUN helper", tr("Checking…", "检查中…"), 0, cMuted);
            statusRow(184, tr("DNS takeover permission", "DNS 接管权限"), tr("Checking…", "检查中…"), 0, cMuted);
            statusRow(220, tr("Network leftovers", "网络残留"), tr("Checking…", "检查中…"), 0, cMuted);
        } else {
            if (!setup.helper.empty()) {
                statusRow(148, "TUN helper", tr("Not installed", "未安装"), -1, cAmber);
            } else if (setup.helperOutdated) {
                statusRow(148, "TUN helper", tr("Older than the bundled mihomo", "比 FrameClash 自带的 mihomo 旧"), -1,
                          cAmber);
            } else {
                statusRow(148, "TUN helper", tr("Ready (/home/.frameclash/mihomo + cap_net_admin)",
                                                "已就绪（/home/.frameclash/mihomo + cap_net_admin）"),
                          1, cGreen);
            }
            if (!setup.selfDns) {
                statusRow(184, tr("DNS takeover permission", "DNS 接管权限"), tr("Not used (self DNS is off)",
                                                                               "未使用（self DNS 已关闭）"),
                          0, cMuted);
            } else if (!setup.haveDnsAllowed) {
                statusRow(184, tr("DNS takeover permission", "DNS 接管权限"), tr("Unknown (pkcheck not found)",
                                                                               "无法检查（找不到 pkcheck）"),
                          0, cMuted);
            } else if (setup.dnsAllowed) {
                statusRow(184, tr("DNS takeover permission", "DNS 接管权限"), tr("Allowed", "已允许"), 1, cGreen);
            } else {
                statusRow(184, tr("DNS takeover permission", "DNS 接管权限"),
                          tr("Missing (an OS update may require install again)", "缺少（系统更新后可能需要重跑 install）"),
                          -1, cAmber);
            }
            if (setup.running) {
                statusRow(220, tr("Network leftovers", "网络残留"), tr("Checked only while the proxy is stopped",
                                                                       "仅在代理停止时检查"),
                          0, cMuted);
            } else if (setup.leftovers.empty()) {
                statusRow(220, tr("Network leftovers", "网络残留"), tr("None", "无"), 1, cGreen);
            } else {
                statusRow(220, tr("Network leftovers", "网络残留"),
                          std::to_string(setup.leftovers.size()) + tr(" items", " 项"), -1, cAmber);
            }
        }
    }

    card(pen, kX0, 262, kCW, 176, tr("RUN IN THE FRAME TERMINAL", "在 Frame 的终端执行"));
    const std::string suggest = setup.have ? setup.suggest : "install";
    if (suggest == "install" || suggest == "cleanup") {
        const std::string command = setupCommand(setup, suggest);
        pen.color(cBg);
        pen.roundedRect(220, 312, 780, 64, 8);
        cairo_fill(pen.cr);
        pen.textMono(236, 352, command, 22, cAmber);
        button(pen, "copy:" + suggest, 1020, 322, 140, 44, tr("Copy", "复制"), true, cAmberTint, cAmber, 18);
        double y = 400;
        for (const std::string& line : wrapText(
                 pen,
                 tr("The script lists its changes and asks y/N. sudo asks for your password (run passwd first if you "
                    "have never set one). Come back and tap Check again afterwards.",
                    "脚本会先列出要做的事并问 y/N，sudo 会要密码（从没设过的话先跑 passwd）。跑完回来按「重新检查」。"),
                 15, false, 920, 2)) {
            pen.text(220, y, line, 15, cMuted);
            y += 24;
        }
    } else if (suggest == "repair") {
        pen.text(220, 330, tr("TUN leftovers can be removed without root.", "无需 root 即可清除 TUN 网络残留。"), 19,
                 cText, true);
        const bool armed = repairArmedAt_ != 0 && nowMs() - repairArmedAt_ < 4000;
        if (!armed) repairArmedAt_ = 0;
        button(pen, "repair", 220, 358, 240, 56, armed ? tr("Tap again to repair", "再点一次确认") :
                                                          tr("Repair network", "修复网络"),
               idle, armed ? cRed : cRedTint, armed ? cOnAmber : cRed, 19);
        pen.text(480, 392, tr("The proxy stays stopped after repair.", "修复后代理保持停止。"), 16, cMuted);
    } else {
        pen.text(220, 342, ICON_CHECK " " + tr("TUN is ready", "TUN 已就绪"), 24, cGreen, true);
        if (!s.status.tun)
            button(pen, "tun", 220, 370, 200, 52, tr("Enable TUN", "开启 TUN"), idle, cAmber, cOnAmber, 19);
    }

    card(pen, kX0, 454, kCW, 170, tr("OTHER COMMANDS", "其他命令"));
    const char* verbs[] = {"no-self-dns", "cleanup", "remove", "status"};
    const char* labels[] = {"install --no-self-dns", "cleanup", "remove", "status"};
    for (int i = 0; i < 4; ++i) {
        // laser targets: 30 px rows are the smallest that stay easy to hit in the headset
        const double y = 516 + i * 32;
        pen.textMono(232, y, setupScript(setup) + " " + labels[i], 16, cText);
        button(pen, std::string("copy:") + verbs[i], 1060, y - 21, 100, 30, tr("Copy", "复制"), true, cCtl, cText, 14);
    }
}
