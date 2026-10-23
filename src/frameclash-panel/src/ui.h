// FrameClash panel (0.5.0 layout: sidebar + Overview / Proxies / Profiles): drawing the 1200x700 dashboard panel and its thumbnail, and turning laser clicks into actions.
// Copyright (c) 2026 FrameClash contributors. MIT License.
#pragma once

#include "model.h"
#include "third_party_frameeyeosc/draw.h"

#include <cairo.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

constexpr int kPanelWidth = 1200;
constexpr int kPanelHeight = 700;
constexpr int kThumbnailSize = 256;

struct Hit {
    std::string id;   ///< e.g. "toggle", "refresh", "settings", "lang", "test", "tab:2", "node:<name>", "up", "down"
    double x, y, w, h;
    bool enabled;
};

/** What the panel needs from the outside for text fields (0.2.1). Any may be empty.
 *  The main way in is Paste (the system clipboard); the stock SteamVR keyboard is an optional extra. */
struct KeyboardHooks {
    /** Open the stock SteamVR keyboard (one ShowKeyboardForOverlay call). @return "" if it opened, else why not */
    std::function<std::string(const std::string& description, const std::string& existing)> show;
    std::function<void()> hide;
    /** Clipboard text ("" + error if none). */
    std::function<std::string(std::string& error)> paste;
};

enum class View { Overview, Proxies, Profiles };

class PanelUi {
public:
    PanelUi(Model& model, const FontSet& fonts, std::function<void()> openSettings, KeyboardHooks keyboard = {});

    /** Draw the panel for this snapshot (also rebuilds the click targets). */
    void draw(cairo_t* cr, const Snapshot& s);
    void drawThumbnail(cairo_t* cr, const Snapshot& s);

    /** Pointer events (panel px, origin top-left). @return true if the panel must be redrawn */
    bool pointerMove(double x, double y);
    bool pointerDown(double x, double y);
    bool pointerUp(double x, double y);
    bool pointerLeave();
    bool scroll(double dy);
    /** Press a target by id (as a click on it would). @return false if there's no such enabled target */
    bool press(const std::string& id);
    /** Show this group's tab. */
    bool showGroup(const std::string& name);

    // ---- 0.2.1: Profiles view and text input ----
    void showView(View view) { view_ = view; }
    View view() const { return view_; }
    /** Optional: open the stock SteamVR keyboard for a field ("url", "name", "path"). @return false if it can't */
    bool startEdit(const std::string& field);
    bool editing() const { return !editing_.empty(); }
    void setField(const std::string& field, const std::string& text);
    std::string field(const std::string& field) const;
    /** Stock SteamVR keyboard events. @return true if the panel must be redrawn */
    bool keyboardChar(const std::string& key);
    bool keyboardDone(const std::string& fullText);
    bool keyboardClosed();

    const std::vector<Hit>& hits() const { return hits_; }
    void setLanguage(bool zh) { zh_ = zh; }
    bool chinese() const { return zh_; }

private:
    Model& model_;
    const FontSet& fonts_;
    std::function<void()> openSettings_;
    KeyboardHooks kbd_;
    View view_ = View::Overview;
    bool importFileMode_ = false;          ///< Profiles view: "Local file" tab (else "Subscription URL")
    std::map<std::string, std::string> fields_;
    std::string editing_;                  ///< field the stock SteamVR keyboard is open for ("" = none)
    int fileOffset_ = 0;
    int profOffset_ = 0;
    std::string deleteArmed_;              ///< profile id waiting for the second "Remove" tap
    int64_t deleteArmedAt_ = 0;
    uint64_t seenImportSeq_ = 0;
    bool seenImportInit_ = false;
    std::string pendingImport_;            ///< "url" / "file" while our import runs (fields clear when it works)
    std::string noteEn_, noteZh_;          ///< the panel's own notes (keyboard fallback, input checks)
    bool noteErr_ = false;
    int64_t noteAtMs_ = 0;
    bool zh_ = false;
    std::string group_;          ///< Proxies: the group shown
    int groupOffset_ = 0;        ///< first group in the list
    int maxGroupOffset_ = 0;
    int rowOffset_ = 0;          ///< first node row shown
    bool sortByDelay_ = false;
    int64_t repairArmedAt_ = 0;  ///< "Repair network" waits for a second tap
    double scrollAccum_ = 0;
    std::string hover_;
    std::string pressed_;
    std::vector<Hit> hits_;
    Snapshot last_;              ///< the snapshot last drawn (to act on what the user saw)
    int maxRowOffset_ = 0;

    std::string tr(const std::string& en, const std::string& zh) const { return zh_ ? zh : en; }
    const Hit* hitAt(double x, double y) const;
    void activate(const std::string& id);
    void addHit(const std::string& id, double x, double y, double w, double h, bool enabled);
    void button(const Pen& pen, const std::string& id, double x, double y, double w, double h,
                const std::string& label, bool enabled, Color fill, Color text, double size = 20);
    void card(const Pen& pen, double x, double y, double w, double h, const std::string& label,
              const std::string& hoverId = "");
    void toggle(const Pen& pen, double x, double y, bool on, bool enabled);
    std::string busyLabel(Busy busy) const;
    void drawSidebar(const Pen& pen, const Snapshot& s);
    void drawOverview(const Pen& pen, const Snapshot& s);
    void drawProxies(const Pen& pen, const Snapshot& s);
    void drawRescue(const Pen& pen, const Snapshot& s);
    int delayOf(const Snapshot& s, const std::string& name) const;
    // ui_profiles.cpp
    void drawProfilesView(const Pen& pen, const Snapshot& s);
    void drawImportCard(const Pen& pen, const Snapshot& s);
    void drawProfilesCard(const Pen& pen, const Snapshot& s);
    void drawField(const Pen& pen, const std::string& id, double x, double y, double w, double h, const std::string& text,
                   const std::string& placeholder, int lines, bool active);
    void drawMessage(const Pen& pen, const Snapshot& s, double x, double y, double w, double h);
    bool activateProfiles(const std::string& id);
    void note(const std::string& en, const std::string& zh, bool error);
    void pasteInto(std::string& target, size_t maxBytes);
    size_t fieldMax(const std::string& field) const;
    void trackImport(const Snapshot& s);
};
