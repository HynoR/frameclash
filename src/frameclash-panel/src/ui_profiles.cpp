// Copyright (c) 2026 FrameClash contributors. MIT License.
// 0.2.1: the Profiles view (import a subscription URL or a YAML file under ~, switch/remove saved profiles).
// Text gets into the fields by Paste (the system clipboard) - typing in VR is not relied on. Tapping a field
// optionally opens the stock SteamVR keyboard (one call); the CLI (`frameclash import-url ...` over SSH) is the
// other way in.
// Typed text and URLs are never logged (subscription URLs carry account tokens).
#include "ui.h"

#include "palette.h"

#include "third_party_frameeyeosc/icons.h"
#include "third_party_frameeyeosc/text_layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kCardTop = 76;
constexpr double kCardBottom = 624;
constexpr double kImpX = 200, kImpW = 480;
constexpr double kProfX = 696, kProfW = 484;
constexpr int kFileRows = 4;
constexpr int kProfRows = 5;
constexpr double kProfRowStep = 72;
constexpr int64_t kNoteMs = 20000;
constexpr int64_t kDeleteArmMs = 3000;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

/** Break mono text into lines of at most `w` pixels at character boundaries, keeping the last `maxLines` lines (the
 *  end of a long URL or path is what you are typing). The first kept line starts with "…" if earlier text is hidden. */
std::vector<std::string> tailLines(const Pen& pen, const std::string& text, double size, double w, size_t maxLines,
                                   bool* cut) {
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < text.size();) {
        const size_t len = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
        const std::string ch = text.substr(i, len);
        if (!lines.back().empty() && pen.measureMono(lines.back() + ch, size) > w) lines.emplace_back();
        lines.back() += ch;
        i += len;
    }
    *cut = lines.size() > maxLines;
    if (*cut) {
        lines.erase(lines.begin(), lines.end() - static_cast<long>(maxLines));
        std::string& first = lines.front();
        size_t drop = 0;
        for (int n = 0; n < 1 && drop < first.size(); ++n) drop += utf8Length(static_cast<unsigned char>(first[drop]));
        first = "…" + first.substr(drop);
    }
    return lines;
}

}  // namespace

size_t PanelUi::fieldMax(const std::string& field) const {
    if (field == "url") return 4096;
    if (field == "name") return 64;
    return 1024;  // path
}

void PanelUi::setField(const std::string& field, const std::string& text) {
    fields_[field] = cleanFieldText(text, fieldMax(field));
}

std::string PanelUi::field(const std::string& field) const {
    auto it = fields_.find(field);
    return it == fields_.end() ? "" : it->second;
}

void PanelUi::note(const std::string& en, const std::string& zh, bool error) {
    noteEn_ = en;
    noteZh_ = zh;
    noteErr_ = error;
    noteAtMs_ = nowMs();
}

void PanelUi::trackImport(const Snapshot& s) {
    if (!seenImportInit_) {
        seenImportInit_ = true;
        seenImportSeq_ = s.importSeq;
        return;
    }
    if (s.importSeq == seenImportSeq_) return;
    seenImportSeq_ = s.importSeq;
    if (s.importOk && pendingImport_ == "url") {
        fields_["url"].clear();
        fields_["name"].clear();
    } else if (s.importOk && pendingImport_ == "file") {
        fields_["path"].clear();
    }
    if (s.importOk) profOffset_ = 0;
    pendingImport_.clear();
}

// ---------------------------------------------------------------- the optional SteamVR keyboard

bool PanelUi::startEdit(const std::string& field) {
    if (field != "url" && field != "name" && field != "path") return false;
    if (!kbd_.show) {
        note("No keyboard here - use Paste, or the frameclash command over SSH.",
             "此处没有键盘——请用「粘贴」，或通过 SSH 使用 frameclash 命令。", true);
        return false;
    }
    const std::string desc = field == "url" ? "Subscription URL" : field == "name" ? "Profile name" : "Config file path";
    const std::string error = kbd_.show(desc, this->field(field));
    if (!error.empty() && !editing_.empty() && error.find("AlreadyInUse") != std::string::npos) {
        // tapped a field while our keyboard is up: keep typing into the field it was opened for
        note("The keyboard is open - type there, then press Done (or close it).",
             "键盘已打开——请在键盘上输入，然后点「完成」（或关闭键盘）。", false);
        return false;
    }
    if (!error.empty()) {
        editing_.clear();
        note("SteamVR keyboard unavailable (" + error + ") - use Paste, or the frameclash command over SSH.",
             "SteamVR 键盘不可用（" + error + "）——请用「粘贴」，或通过 SSH 使用 frameclash 命令。", true);
        return false;
    }
    editing_ = field;
    return true;
}

bool PanelUi::keyboardDone(const std::string& fullText) {
    if (editing_.empty()) return false;
    const std::string text = cleanFieldText(fullText, fieldMax(editing_));
    if (!text.empty()) fields_[editing_] = text;  // the keyboard's whole text replaces the field
    editing_.clear();
    return true;
}

bool PanelUi::keyboardChar(const std::string& key) {
    if (editing_.empty() || key.empty()) return false;
    std::string& f = fields_[editing_];
    if (key == "\b" || key == "\x7f") {
        utf8PopBack(f);
    } else if (key == "\n" || key == "\r") {  // Enter: finished
        if (kbd_.hide) kbd_.hide();
        editing_.clear();
    } else if (key[0] != '\x1b') {  // arrow keys come as escape sequences: no cursor here, ignore them
        // appended as typed (cleanFieldText would trim a space typed between two words)
        const bool printable = std::all_of(key.begin(), key.end(), [](char c) {
            return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
        });
        if (printable && f.size() + key.size() <= fieldMax(editing_)) f += key;
    }
    return true;
}

bool PanelUi::keyboardClosed() {
    if (editing_.empty()) return false;
    editing_.clear();
    return true;
}

void PanelUi::pasteInto(std::string& target, size_t maxBytes) {
    if (!kbd_.paste) {
        note("Paste is not available here.", "此处无法粘贴。", true);
        return;
    }
    std::string error;
    const std::string text = cleanFieldText(trim(kbd_.paste(error)), maxBytes);
    if (text.empty()) {
        note("Nothing to paste: " + (error.empty() ? std::string("clipboard empty") : error) +
                 ". Copy the text first, or use: frameclash import-url … over SSH.",
             "无法粘贴：" + (error.empty() ? std::string("剪贴板为空") : error) +
                 "。请先复制，或通过 SSH 使用 frameclash import-url …",
             true);
        return;
    }
    target = text;  // paste replaces the whole field
    note("Pasted " + std::to_string(text.size()) + " characters.", "已粘贴 " + std::to_string(text.size()) + " 个字符。",
         false);
}

// ---------------------------------------------------------------- actions

bool PanelUi::activateProfiles(const std::string& id) {
    if (id == "imode:url") {
        importFileMode_ = false;
    } else if (id == "imode:file") {
        importFileMode_ = true;
    } else if (id.rfind("field:", 0) == 0) {
        startEdit(id.substr(6));
    } else if (id.rfind("clear:", 0) == 0) {
        fields_[id.substr(6)].clear();
    } else if (id.rfind("paste:", 0) == 0) {
        const std::string f = id.substr(6);
        pasteInto(fields_[f], fieldMax(f));
    } else if (id == "import:url") {
        const std::string url = trim(field("url"));
        const std::string l = lower(url);
        if (url.empty()) {
            note("Enter the subscription URL first.", "请先输入订阅链接。", true);
        } else if (l.rfind("http://", 0) != 0 && l.rfind("https://", 0) != 0) {
            note("The URL must start with http:// or https://", "链接必须以 http:// 或 https:// 开头", true);
        } else if (url.find(' ') != std::string::npos) {
            note("The URL contains a space - check it.", "链接中含有空格，请检查。", true);
        } else {
            pendingImport_ = "url";
            model_.importUrl(url, trim(field("name")));
        }
    } else if (id == "import:file") {
        const std::string path = trim(field("path"));
        if (path.empty()) {
            note("Enter a path or pick a file below.", "请输入路径或在下方选择文件。", true);
        } else {
            pendingImport_ = "file";
            model_.importFile(path, "");
        }
    } else if (id.rfind("file:", 0) == 0) {
        const size_t i = std::stoul(id.substr(5));
        if (i < last_.profiles.files.size()) fields_["path"] = last_.profiles.files[i].path;
    } else if (id == "filesup") {
        fileOffset_ = std::max(0, fileOffset_ - kFileRows);
    } else if (id == "filesdown") {
        fileOffset_ += kFileRows;
    } else if (id == "profup") {
        profOffset_ = std::max(0, profOffset_ - kProfRows);
    } else if (id == "profdown") {
        profOffset_ += kProfRows;
    } else if (id.rfind("prof:use:", 0) == 0) {
        model_.activateProfile(id.substr(9));
    } else if (id.rfind("prof:del:", 0) == 0) {
        const std::string pid = id.substr(9);
        if (deleteArmed_ == pid && nowMs() - deleteArmedAt_ < kDeleteArmMs) {
            deleteArmed_.clear();
            model_.deleteProfile(pid);
        } else {
            deleteArmed_ = pid;
            deleteArmedAt_ = nowMs();
        }
    } else if (id == "kbd:hide") {
        if (kbd_.hide) kbd_.hide();
        editing_.clear();
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- drawing

void PanelUi::drawField(const Pen& pen, const std::string& id, double x, double y, double w, double h,
                        const std::string& text, const std::string& placeholder, int lines, bool active) {
    const bool hot = hover_ == id || pressed_ == id;
    pen.color(hot ? cCtlHot : cBg);
    pen.roundedRect(x, y, w, h, 10);
    cairo_fill(pen.cr);
    pen.color(active ? cAmber : hot ? cAmber : cDim);
    cairo_set_line_width(pen.cr, active ? 3 : 2);
    pen.roundedRect(x + 1, y + 1, w - 2, h - 2, 9);
    cairo_stroke(pen.cr);
    const double size = 19;
    if (text.empty()) {
        pen.text(x + 16, y + (lines == 1 ? h / 2 + 7 : 34), placeholder, 18, cDim);
    } else {
        bool cut = false;
        const auto shown = tailLines(pen, text, size, w - 32 - 72, static_cast<size_t>(lines), &cut);
        double ly = lines == 1 ? y + h / 2 + 7 : y + 34;
        for (const std::string& line : shown) {
            pen.textMono(x + 16, ly, line, size, cText);
            ly += 28;
        }
    }
    // tapping a field opens the stock SteamVR keyboard (optional; Paste is the main way in)
    if (kbd_.show)
        pen.text(x + w - 14, y + (lines == 1 ? h / 2 + 6 : 32), active ? tr("typing…", "输入中…") : tr("Type", "键入"), 14,
                 active ? cAmber : cMuted, true, true);
    addHit(id, x, y, w, h, kbd_.show != nullptr);
}

void PanelUi::drawMessage(const Pen& pen, const Snapshot& s, double x, double y, double w, double h) {
    std::string msg;
    bool err = false;
    const int64_t now = nowMs();
    const bool own = !noteEn_.empty() && now - noteAtMs_ < kNoteMs && noteAtMs_ >= s.messageAtMs;
    if (own) {
        msg = zh_ ? noteZh_ : noteEn_;
        err = noteErr_;
    } else if (!s.message.empty()) {
        msg = zh_ ? s.messageZh : s.message;
        err = s.messageIsError;
    } else if (s.reach == Reach::Unreachable && view_ == View::Profiles) {
        msg = tr("frameclashd is not answering - nothing can be imported right now.", "frameclashd 无响应，暂时无法导入。");
        err = true;
    }
    if (msg.empty()) return;
    pen.color(err ? cRedTint : cCard);
    pen.roundedRect(x, y, w, h, 8);
    cairo_fill(pen.cr);
    const std::string line = ellipsize(pen, (err ? ICON_CROSS " " : ICON_CHECK " ") + msg, 16, false, w - 32, false);
    pen.text(x + 16, y + h / 2 + 6, line, 16, err ? cRed : cText);
}

void PanelUi::drawProfilesView(const Pen& pen, const Snapshot& s) {
    card(pen, kImpX, kCardTop, kImpW, kCardBottom - kCardTop, "");
    card(pen, kProfX, kCardTop, kProfW, kCardBottom - kCardTop, "");
    drawImportCard(pen, s);
    drawProfilesCard(pen, s);
}

void PanelUi::drawImportCard(const Pen& pen, const Snapshot& s) {
    const double x = kImpX + 20, w = kImpW - 40;
    pen.text(x, kCardTop + 40, tr("IMPORT A PROFILE", "导入配置"), 16, cMuted, true);
    const bool can = s.reach == Reach::Ok && s.busy == Busy::None;
    const double tabW = (w - 8) / 2;
    button(pen, "imode:url", x, 136, tabW, 44, tr("Subscription URL", "订阅链接"), true,
           importFileMode_ ? cCtl : cAmberTint, importFileMode_ ? cText : cAmber, 18);
    button(pen, "imode:file", x + tabW + 8, 136, tabW, 44, tr("Config file", "配置文件"), true,
           importFileMode_ ? cAmberTint : cCtl, importFileMode_ ? cAmber : cText, 18);
    const bool importing = s.busy == Busy::Importing;
    const double fw = w - 150, px = x + w - 140;  // field width, Paste button x
    if (!importFileMode_) {
        pen.text(x, 212, tr("Subscription URL", "订阅链接"), 17, cMuted);
        drawField(pen, "field:url", x, 224, fw, 92, field("url"), tr("Copy the link, then Paste", "复制链接后点「粘贴」"), 2,
                  editing_ == "url");
        button(pen, "paste:url", px, 224, 140, 52, tr("Paste", "粘贴"), true, cAmberTint, cAmber, 19);
        button(pen, "clear:url", px, 282, 140, 34, tr("Clear", "清空"), !field("url").empty(), cCtl, cText, 15);
        pen.text(x, 340, std::to_string(field("url").size()) + tr(" chars", " 字符"), 14, cMuted);
        pen.text(x, 380, tr("Name (optional)", "名称（可选）"), 17, cMuted);
        drawField(pen, "field:name", x, 392, fw, 52, field("name"), tr("e.g. Home", "例如：家里"), 1, editing_ == "name");
        button(pen, "paste:name", px, 392, 140, 52, tr("Paste", "粘贴"), true, cCtl, cText, 18);
        button(pen, "import:url", x, 466, w, 56,
               importing ? tr("Importing…", "正在导入…") : ICON_ARROW_RIGHT " " + tr("Import subscription", "导入订阅"),
               can && !field("url").empty(), cAmber, cOnAmber, 21);
    } else {
        pen.text(x, 212, tr("YAML file in your home folder", "主目录中的 YAML 文件"), 17, cMuted);
        drawField(pen, "field:path", x, 224, fw, 52, field("path"), "~/Downloads/config.yaml", 1, editing_ == "path");
        button(pen, "paste:path", px, 224, 140, 52, tr("Paste", "粘贴"), true, cAmberTint, cAmber, 19);
        button(pen, "clear:path", x, 286, 120, 44, tr("Clear", "清空"), !field("path").empty(), cCtl, cText, 17);
        button(pen, "import:file", x + 130, 286, w - 130, 44,
               importing ? tr("Importing…", "正在导入…") : ICON_ARROW_RIGHT " " + tr("Import file", "导入文件"),
               can && !field("path").empty(), cAmber, cOnAmber, 19);
        const auto& files = s.profiles.files;
        const int n = static_cast<int>(files.size());
        fileOffset_ = std::max(0, std::min(fileOffset_, ((n - 1) / kFileRows) * kFileRows));
        const std::string where = s.profiles.dir.empty() ? "~/.config/frameclash/profiles" : s.profiles.dir;
        pen.text(x, 364, ellipsize(pen, tr("Found in ", "位于 ") + where + tr(" and ~/Downloads", " 和 ~/Downloads"), 15,
                                   false, w - 100, false),
                 15, cMuted);
        if (n > kFileRows) {
            button(pen, "filesup", x + w - 88, 340, 40, 32, ICON_TRIANGLE_UP, fileOffset_ > 0, cCtl, cText, 14);
            button(pen, "filesdown", x + w - 40, 340, 40, 32, ICON_TRIANGLE_DOWN, fileOffset_ + kFileRows < n, cCtl,
                   cText, 14);
        }
        if (files.empty()) {
            double ly = 404;
            for (const std::string& line :
                 wrapText(pen, tr("No .yaml/.yml files found. Copy one into the folder above (e.g. from Desktop Mode), "
                                  "or type its path.",
                                  "未找到 .yaml/.yml 文件。可（在桌面模式中）复制到上述目录，或输入路径。"),
                          16, false, w, 3)) {
                pen.text(x, ly, line, 16, cMuted);
                ly += 24;
            }
        }
        for (int r = 0; r < kFileRows && fileOffset_ + r < n; ++r) {
            const int i = fileOffset_ + r;
            const YamlFile& f = files[static_cast<size_t>(i)];
            const double ry = 380 + r * 44;
            const std::string id = "file:" + std::to_string(i);
            const bool chosen = field("path") == f.path;
            const bool hot = hover_ == id || pressed_ == id;
            pen.color(chosen ? cAmberTint : hot ? cCtlHot : cCtl);
            pen.roundedRect(x, ry, w, 40, 8);
            cairo_fill(pen.cr);
            const std::string meta = f.modified.size() >= 10 ? f.modified.substr(0, 10) : f.modified;
            const double mw = pen.text(x + w - 12, ry + 26, meta, 14, cMuted, false, true);
            pen.text(x + 12, ry + 27, ellipsize(pen, f.path, 17, false, w - 36 - mw, true), 17, chosen ? cAmber : cText);
            addHit(id, x, ry, w, 40, true);
        }
    }
    // the other ways in: the CLI over SSH, and the full settings page (secondary)
    pen.text(x, 556, tr("Over SSH:", "SSH："), 15, cMuted);
    pen.textMono(x, 580, importFileMode_ ? "frameclash import-file PATH" : "frameclash import-url URL", 15, cAmber);
    button(pen, "settings", x + w - 190, 552, 190, 40, tr("Full settings", "完整设置") + " " ICON_CHEVRON_RIGHT, true,
           cCtl, cMuted, 16);
}

void PanelUi::drawProfilesCard(const Pen& pen, const Snapshot& s) {
    const double x = kProfX + 20, w = kProfW - 40;
    const auto& list = s.profiles.profiles;
    const int n = static_cast<int>(list.size());
    pen.text(x, kCardTop + 40, tr("SAVED PROFILES", "已保存的配置"), 16, cMuted, true);
    if (n > 0) pen.text(x + w, kCardTop + 40, std::to_string(n), 16, cMuted, true, true);
    if (deleteArmed_.size() && nowMs() - deleteArmedAt_ >= kDeleteArmMs) deleteArmed_.clear();
    profOffset_ = std::max(0, std::min(profOffset_, std::max(0, n - kProfRows)));
    const bool idle = s.reach == Reach::Ok && s.busy == Busy::None;
    if (n == 0) {
        double ly = 180;
        const std::string msg = !s.haveProfiles && s.reach != Reach::Ok
                                    ? tr("Waiting for frameclashd…", "等待 frameclashd…")
                                    : tr("Nothing saved yet. Every subscription or file you import is kept here, so "
                                         "you can switch between them later.",
                                         "还没有保存的配置。导入的订阅和文件都会保存在这里，之后可随时切换。");
        for (const std::string& line : wrapText(pen, msg, 18, false, w, 4)) {
            pen.text(x, ly, line, 18, cMuted);
            ly += 28;
        }
    }
    for (int r = 0; r < kProfRows && profOffset_ + r < n; ++r) {
        const ProfileEntry& p = list[static_cast<size_t>(profOffset_ + r)];
        const double ry = 128 + r * kProfRowStep;
        pen.color(p.active ? cAmberTint : cCtl);
        pen.roundedRect(x, ry, w, kProfRowStep - 8, 10);
        cairo_fill(pen.cr);
        const double bw = 92, bx2 = x + w - 12 - bw, bx1 = bx2 - 10 - bw;
        const double tw = bx1 - x - 28;
        double nx = x + 16;
        const std::string badge = tr("ACTIVE", "使用中");
        const double badgeW = p.active ? pen.measure(badge, 13, true) + 18 : 0;
        const std::string name = ellipsize(pen, p.name.empty() ? "—" : p.name, 19, true, tw - badgeW - 8, false);
        nx += pen.text(nx, ry + 28, name, 19, cText, true) + 10;
        if (p.active) {
            pen.color(cAmber);
            pen.roundedRect(nx, ry + 11, badgeW, 22, 11);
            cairo_fill(pen.cr);
            pen.text(nx + 9, ry + 27, badge, 13, cOnAmber, true);
        }
        const std::string kind = p.kind == "url" ? tr("Subscription", "订阅") : tr("File", "文件");
        std::string updated = p.updated;
        if (updated.size() > 16 && updated[10] == 'T') updated = updated.substr(5, 5) + " " + updated.substr(11, 5);
        const double kw = pen.text(x + 16, ry + 54, kind + " · ", 15, cMuted);
        const double uw = pen.measure(updated, 14, false);
        pen.text(x + 16 + kw, ry + 54,
                 ellipsize(pen, p.where.empty() ? tr("pasted YAML", "粘贴的 YAML") : p.where, 15, false,
                           tw - kw - uw - 12, p.kind == "file"),
                 15, cMuted);
        pen.text(bx1 - 12, ry + 54, updated, 14, cMuted, false, true);
        const bool busyThis = (s.busy == Busy::Activating || s.busy == Busy::Deleting) && s.testingGroup == p.id;
        const std::string use = busyThis && s.busy == Busy::Activating ? tr("Loading…", "载入中…")
                                : p.active                            ? tr("Refresh", "更新")
                                                                      : tr("Use", "使用");
        button(pen, "prof:use:" + p.id, bx1, ry + 11, bw, 46, use, idle, p.active ? cCtl : cAmber,
               p.active ? cText : cOnAmber, 17);
        const bool armed = deleteArmed_ == p.id;
        button(pen, "prof:del:" + p.id, bx2, ry + 11, bw, 46,
               busyThis && s.busy == Busy::Deleting ? tr("Removing…", "移除中…")
               : armed                              ? tr("Sure?", "确定？")
                                                    : tr("Remove", "移除"),
               idle && !p.active, armed ? cRed : cRedTint, armed ? cOnAmber : cRed, 17);
    }
    if (n > kProfRows) {
        button(pen, "profup", x, 496, 120, 40, ICON_TRIANGLE_UP " " + tr("Up", "上"), profOffset_ > 0, cCtl, cText, 16);
        button(pen, "profdown", x + 130, 496, 120, 40, ICON_TRIANGLE_DOWN " " + tr("Down", "下"),
               profOffset_ + kProfRows < n, cCtl, cText, 16);
        pen.text(x + w, 522,
                 std::to_string(profOffset_ + 1) + "–" + std::to_string(std::min(n, profOffset_ + kProfRows)) + " / " +
                     std::to_string(n),
                 15, cMuted, false, true);
    }
    double ly = 566;
    for (const std::string& line :
         wrapText(pen, tr("\"Use\" switches to a profile (a subscription is downloaded again). \"Remove\" only takes it "
                          "off this list - your files are never deleted.",
                          "「使用」切换到该配置（订阅会重新下载）。「移除」只是从列表中删除，不会删除你的文件。"),
                  15, false, w, 3)) {
        pen.text(x, ly, line, 15, cMuted);
        ly += 22;
    }
}

