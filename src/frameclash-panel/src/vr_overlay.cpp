// Implementation of the OpenVR connection and the dashboard panel.
// Adapted from frameeyeosc's panel/src/vr_overlay.cpp (https://github.com/sasaken1102r/frameeyeosc, commit c4294a9).
// Copyright (c) 2026 konsti219; Copyright (c) 2026 sasaken1102r (original). MIT License:
// third_party_frameeyeosc/LICENSE.frameeyeosc. Changes for FrameClash are under the same license.
#include "vr_overlay.h"

#include "openvr.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

constexpr const char* kDashboardKey = "frameclash.panel";
constexpr const char* kDashboardName = "FrameClash";
// Same px-to-meter ratio as frameeyeosc's 1200 px / 2.8 m panel
constexpr float kDashboardWidthM = 2.8f;
// After clearing the textures, wait a few compositor frames before VR_Shutdown (frameeyeosc's value)
constexpr int kShutdownWaitMs = 400;
constexpr double kSmoothScrollPx = 120.0;
constexpr double kDiscreteScrollPx = 80.0;

const char* overlayErrorName(vr::EVROverlayError error) {
    return vr::VROverlay()->GetOverlayErrorNameFromEnum(error);
}

bool checkOverlay(const char* what, vr::EVROverlayError error) {
    if (error == vr::VROverlayError_None) return true;
    std::fprintf(stderr, "[VR] %s failed: %s\n", what, overlayErrorName(error));
    return false;
}

}  // namespace

VrOverlay::~VrOverlay() {
    shutdown();
}

int VrOverlay::findVrserverPid() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return -1;
    int found = -1;
    while (const dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') continue;
        const std::string commPath = std::string("/proc/") + name + "/comm";
        const int fd = ::open(commPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char comm[64] = {};
        const ssize_t n = ::read(fd, comm, sizeof(comm) - 1);
        ::close(fd);
        if (n > 0 && std::strncmp(comm, "vrserver\n", 9) == 0) {
            found = std::atoi(name);
            break;
        }
    }
    ::closedir(proc);
    return found;
}

VrOverlay::ConnectResult VrOverlay::connect(int width, int height, std::string& message) {
    if (connected_) return ConnectResult::Ok;
    // 1) Background-type init only checks that SteamVR runs (never launches it)
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return error == vr::VRInitError_Init_NoServerForBackgroundApp ? ConnectResult::NotRunning
                                                                        : ConnectResult::Error;
    }
    vr::VR_Shutdown();
    // 2) Reconnect as an overlay app
    vr::VR_Init(&error, vr::VRApplication_Overlay);
    if (error != vr::VRInitError_None) {
        message = vr::VR_GetVRInitErrorAsEnglishDescription(error);
        return ConnectResult::Error;
    }
    connected_ = true;
    // 3) Vulkan on the HMD's GPU
    std::string vkMessage;
    if (!vulkan_.init(vkMessage)) {
        message = "Vulkan setup failed: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }
    // 4) Dashboard panel and thumbnail
    vr::IVROverlay* overlay = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail = vr::k_ulOverlayHandleInvalid;
    const vr::EVROverlayError createError =
        overlay->CreateDashboardOverlay(kDashboardKey, kDashboardName, &main, &thumbnail);
    std::fprintf(stderr, "[VR] CreateDashboardOverlay(%s) -> %s\n", kDashboardKey, overlayErrorName(createError));
    if (createError != vr::VROverlayError_None) {
        message = std::string("CreateDashboardOverlay: ") + overlayErrorName(createError);
        shutdown();
        return ConnectResult::Error;
    }
    dashboardHandle_ = main;
    thumbnailHandle_ = thumbnail;
    panelHeight_ = height;
    checkOverlay("SetOverlayWidthInMeters", overlay->SetOverlayWidthInMeters(main, kDashboardWidthM));
    checkOverlay("SetOverlayInputMethod", overlay->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
    const vr::HmdVector2_t scale = {{static_cast<float>(width), static_cast<float>(height)}};
    checkOverlay("SetOverlayMouseScale", overlay->SetOverlayMouseScale(main, &scale));
    checkOverlay("SetOverlayFlag(EnableControlBarClose)",
                 overlay->SetOverlayFlag(main, vr::VROverlayFlags_EnableControlBarClose, true));
    // OpenVR >= 2.15.6 draws a grey backside on overlays by default; the panel is front-only (Valve release notes)
    checkOverlay("SetOverlayFlag(NoBackside)", overlay->SetOverlayFlag(main, vr::VROverlayFlags_NoBackside, true));
    if (!panelTexture_.create(vulkan_, width, height, vkMessage)) {
        message = "Failed to create the panel texture: " + vkMessage;
        shutdown();
        return ConnectResult::Error;
    }
    vrserverPid_ = findVrserverPid();
    lastPanelError_.clear();
    return ConnectResult::Ok;
}

void VrOverlay::setPanelScroll(bool on) {
    if (!connected_ || dashboardHandle_ == 0 || on == panelScroll_) return;
    panelScroll_ = on;
    checkOverlay("SetOverlayFlag(SendVRSmoothScrollEvents)",
                 vr::VROverlay()->SetOverlayFlag(dashboardHandle_, vr::VROverlayFlags_SendVRSmoothScrollEvents, on));
}

void VrOverlay::shutdown() {
    if (!connected_) return;
    vr::IVROverlay* overlay = vr::VROverlay();
    std::fprintf(stderr, "[VR] shutdown\n");
    hideKeyboard();  // an open keyboard outlives its process and blocks every later ShowKeyboardForOverlay
    // Same order as frameeyeosc: clear textures, destroy overlays, wait, VR_Shutdown, then free Vulkan
    if (dashboardHandle_ != 0) {
        overlay->ClearOverlayTexture(dashboardHandle_);
        overlay->ClearOverlayTexture(thumbnailHandle_);
        overlay->DestroyOverlay(dashboardHandle_);
    }
    dashboardHandle_ = 0;
    thumbnailHandle_ = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownWaitMs));
    vr::VR_Shutdown();
    connected_ = false;
    panelScroll_ = false;
    thumbnailTexture_.destroy();
    panelTexture_.destroy();
    vulkan_.destroy();
}

VrEvents VrOverlay::pollEvents() {
    VrEvents result;
    if (!connected_) return result;
    vr::VREvent_t event {};
    while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) result.quit = true;
        if (event.eventType >= vr::VREvent_KeyboardClosed && event.eventType <= vr::VREvent_KeyboardClosed_Global)
            keyboardEvent(&event, result, false);
    }
    if (dashboardHandle_ != 0) {
        while (vr::VROverlay()->PollNextOverlayEvent(dashboardHandle_, &event, sizeof(event))) {
            // Mouse origin is bottom-left; flip to top-left
            const double x = event.data.mouse.x;
            const double y = panelHeight_ - event.data.mouse.y;
            switch (event.eventType) {
                case vr::VREvent_MouseMove: result.pointer.push_back({PointerInput::Type::Move, x, y}); break;
                case vr::VREvent_MouseButtonDown:
                    if (event.data.mouse.button == vr::VRMouseButton_Left)
                        result.pointer.push_back({PointerInput::Type::Down, x, y});
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (event.data.mouse.button == vr::VRMouseButton_Left)
                        result.pointer.push_back({PointerInput::Type::Up, x, y});
                    break;
                case vr::VREvent_FocusLeave: result.pointer.push_back({PointerInput::Type::Leave, 0, 0}); break;
                case vr::VREvent_ScrollSmooth:
                case vr::VREvent_ScrollDiscrete: {
                    const bool smooth = event.eventType == vr::VREvent_ScrollSmooth;
                    const double dy = -event.data.scroll.ydelta * (smooth ? kSmoothScrollPx : kDiscreteScrollPx);
                    if (dy != 0.0) result.pointer.push_back({PointerInput::Type::Scroll, 0, dy});
                    break;
                }
                case vr::VREvent_KeyboardClosed:
                case vr::VREvent_KeyboardCharInput:
                case vr::VREvent_KeyboardDone:
                    keyboardEvent(&event, result, true);
                    break;
                case vr::VREvent_OverlayClosed:
                    std::fprintf(stderr, "[VR] dashboard \"close\" pressed\n");
                    result.closeRequested = true;
                    break;
                default: break;
            }
        }
    }
    if (result.quit) {
        std::fprintf(stderr, "[VR] SteamVR is quitting\n");
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
    return result;
}

std::string VrOverlay::showKeyboard(const std::string& description, const std::string& existing, uint32_t maxChars) {
    if (!connected_ || dashboardHandle_ == 0) return "not connected to SteamVR";
    vr::IVROverlay* overlay = vr::VROverlay();
    const vr::EVROverlayError error = overlay->ShowKeyboardForOverlay(
        dashboardHandle_, vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine,
        vr::KeyboardFlag_Minimal,
        description.c_str(), maxChars, existing.c_str(), keyboardToken_ + 1);
    std::fprintf(stderr, "[VR] ShowKeyboardForOverlay -> %s\n", overlayErrorName(error));
    // a failed call (e.g. KeyboardAlreadyInUse: ours is still up) leaves the open keyboard and its token alone,
    // so its Done still reaches the field
    if (error == vr::VROverlayError_KeyboardAlreadyInUse && !keyboardOpen_) {
        // not ours (e.g. left open by a panel that was restarted): free it. Not re-shown at once - the Closed event
        // of the old one would then end the new session
        overlay->HideKeyboard();
        return "the keyboard was busy and has been closed - tap the field again";
    }
    if (error != vr::VROverlayError_None) return overlayErrorName(error);
    ++keyboardToken_;
    keyboardChars_ = 0;
    vr::HmdRect2_t avoid {};  // after Show (OpenVR #1372): pins the keyboard below the dashboard, not behind it
    overlay->SetKeyboardPositionForOverlay(dashboardHandle_, avoid);
    keyboardOpen_ = true;
    return "";
}

std::string VrOverlay::keyboardText() const {
    if (!connected_) return "";
    std::vector<char> buffer(8192, 0);
    vr::VROverlay()->GetKeyboardText(buffer.data(), static_cast<uint32_t>(buffer.size()));
    return std::string(buffer.data(), strnlen(buffer.data(), buffer.size()));
}

void VrOverlay::hideKeyboard() {
    if (!connected_ || !keyboardOpen_) return;
    vr::VROverlay()->HideKeyboard();
    keyboardOpen_ = false;
}

void VrOverlay::keyboardEvent(const void* raw, VrEvents& out, bool overlayQueue) {
    const vr::VREvent_t& event = *static_cast<const vr::VREvent_t*>(raw);
    if (!keyboardOpen_) return;  // Done/Closed may come on both queues: the first one counts
    const vr::VREvent_Keyboard_t& k = event.data.keyboard;
    if (event.eventType == vr::VREvent_KeyboardCharInput) {
        // minimal mode: one event per key, on both queues - only the overlay one counts, or every key lands twice
        if (overlayQueue) {
            ++keyboardChars_;
            out.keyboard.push_back({KeyboardInput::Type::Char, std::string(k.cNewInput, strnlen(k.cNewInput, 8))});
        }
    } else if (event.eventType == vr::VREvent_KeyboardDone && k.uUserValue == keyboardToken_) {
        std::fprintf(stderr, "[VR] keyboard Done (%d key events)\n", keyboardChars_);  // never the text
        out.keyboard.push_back({KeyboardInput::Type::Done, keyboardText()});
        keyboardOpen_ = false;
    } else if (event.eventType == vr::VREvent_KeyboardClosed ||
               (event.eventType == vr::VREvent_KeyboardClosed_Global && k.overlayHandle == dashboardHandle_)) {
        std::fprintf(stderr, "[VR] keyboard closed (%d key events)\n", keyboardChars_);
        out.keyboard.push_back({KeyboardInput::Type::Closed, ""});
        keyboardOpen_ = false;
    }
}

bool VrOverlay::steamVrAlive() const {
    if (vrserverPid_ <= 0) return true;
    return ::kill(vrserverPid_, 0) == 0 || errno == EPERM;
}

bool VrOverlay::panelVisible() const {
    return connected_ && dashboardHandle_ != 0 && vr::VROverlay()->IsOverlayVisible(dashboardHandle_);
}

void VrOverlay::showPanel() {
    if (!connected_ || dashboardHandle_ == 0) return;
    vr::VROverlay()->ShowDashboard(kDashboardKey);
    std::fprintf(stderr, "[VR] ShowDashboard(%s)\n", kDashboardKey);
}

bool VrOverlay::submitPanel(const uint8_t* rgba) {
    if (!connected_ || dashboardHandle_ == 0 || !panelTexture_.ready()) return false;
    std::string message;
    const bool ok = panelTexture_.update(dashboardHandle_, rgba, message);
    if (message != lastPanelError_) {
        if (!ok) std::fprintf(stderr, "[VR] can't send the panel: %s\n", message.c_str());
        lastPanelError_ = message;
    }
    return ok;
}

bool VrOverlay::submitThumbnail(const uint8_t* rgba, int size) {
    if (!connected_ || thumbnailHandle_ == 0) return false;
    std::string message;
    if (!thumbnailTexture_.ready() && !thumbnailTexture_.create(vulkan_, size, size, message)) {
        std::fprintf(stderr, "[VR] thumbnail texture: %s\n", message.c_str());
        return false;
    }
    if (!thumbnailTexture_.update(thumbnailHandle_, rgba, message)) {
        std::fprintf(stderr, "[VR] can't send the thumbnail: %s\n", message.c_str());
        return false;
    }
    return true;
}
