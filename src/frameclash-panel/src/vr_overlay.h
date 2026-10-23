// OpenVR connection and the FrameClash dashboard panel (plus its thumbnail).
// Adapted from frameeyeosc's panel/src/vr_overlay.h (https://github.com/sasaken1102r/frameeyeosc, commit c4294a9):
// kept the dashboard-overlay part (connect, events, show, submit, shutdown order); the eye-fit target, gaze dots and
// eye-capture overlays were removed; VROverlayFlags_NoBackside added (OpenVR 2.15.6 draws backsides by default).
// Copyright (c) 2026 konsti219; Copyright (c) 2026 sasaken1102r (original). MIT License:
// third_party_frameeyeosc/LICENSE.frameeyeosc. Changes for FrameClash are under the same license.
#pragma once

#include "third_party_frameeyeosc/vk_texture.h"

#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

/** A pointer action on the panel (px, origin top-left). For Scroll, y = px to scroll (positive = further down). */
struct PointerInput {
    enum class Type { Move, Down, Up, Leave, Scroll };
    Type type;
    double x = 0.0;
    double y = 0.0;
};

/** Stock SteamVR keyboard result (0.2.1, optional feature). The text itself is never logged. */
struct KeyboardInput {
    enum class Type { Char, Done, Closed };
    Type type;
    std::string text;     ///< Char: one key (UTF-8, "\b" = backspace, "\n" = enter); Done: GetKeyboardText() (may be "")
};

struct VrEvents {
    bool quit = false;            ///< SteamVR is shutting down (VREvent_Quit)
    bool closeRequested = false;  ///< the dashboard icon bar's "close" (VREvent_OverlayClosed)
    std::vector<PointerInput> pointer;
    std::vector<KeyboardInput> keyboard;
};

class VrOverlay {
public:
    enum class ConnectResult { Ok, NotRunning, Error };

    VrOverlay() = default;
    ~VrOverlay();
    VrOverlay(const VrOverlay&) = delete;
    VrOverlay& operator=(const VrOverlay&) = delete;

    /** Connect (only if SteamVR is already running), set up Vulkan and the dashboard panel. */
    ConnectResult connect(int width, int height, std::string& message);
    void shutdown();
    VrEvents pollEvents();
    bool steamVrAlive() const;
    bool panelVisible() const;
    void showPanel();
    void setPanelScroll(bool on);
    bool connected() const { return connected_; }
    bool submitPanel(const uint8_t* rgba);
    bool submitThumbnail(const uint8_t* rgba, int size);

    /** Optional: the stock SteamVR keyboard for the panel - one ShowKeyboardForOverlay(panel, Normal, SingleLine,
     *  flags 0, ...) call, then SetKeyboardPositionForOverlay(panel, {0,0,0,0}) so it does not open behind the
     *  dashboard. No other workarounds: Paste and the CLI are the supported ways to enter text.
     *  @return "" when shown, else the OpenVR error */
    std::string showKeyboard(const std::string& description, const std::string& existing, uint32_t maxChars);
    void hideKeyboard();
    bool keyboardOpen() const { return keyboardOpen_; }

private:
    bool connected_ = false;
    uint64_t dashboardHandle_ = 0;
    uint64_t thumbnailHandle_ = 0;
    int panelHeight_ = 0;
    bool panelScroll_ = false;
    int vrserverPid_ = -1;
    std::string lastPanelError_;
    bool keyboardOpen_ = false;
    uint64_t keyboardToken_ = static_cast<uint64_t>(::getpid()) << 32;      ///< uUserValue of the current session (events of older sessions are dropped)
    std::string keyboardText() const;
    void keyboardEvent(const void* event, VrEvents& out, bool overlayQueue);
    int keyboardChars_ = 0;           ///< key events of the current session (logged as a count, never the text)
    VulkanContext vulkan_;
    OverlayTexture panelTexture_;
    OverlayTexture thumbnailTexture_;

    static int findVrserverPid();
};
