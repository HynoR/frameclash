// frameclash-panel: a SteamVR dashboard panel for FrameClash (a thin client of frameclashd's loopback HTTP API).
// Structure (VR connect / retry, single instance, render-when-dirty, exit 3 = closed by the user) follows
// frameeyeosc's panel (MIT, Copyright (c) 2026 konsti219; Copyright (c) 2026 sasaken1102r).
// Copyright (c) 2026 FrameClash contributors. MIT License.
#include "model.h"
#include "palette.h"
#include "spawn.h"
#include "third_party_frameeyeosc/theme.h"
#include "ui.h"
#include "vr_overlay.h"

#include <cairo.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifndef FRAMECLASH_PANEL_VERSION
#define FRAMECLASH_PANEL_VERSION "0.2.1"
#endif

namespace {

constexpr int kExitClosed = 3;        // closed by the user / already running: systemd must not restart it
constexpr int kRetryConnectMs = 5000;
constexpr const char* kInstanceSocket = "frameclash-panel";  // abstract unix socket name

std::atomic<bool> gStop {false};

void onSignal(int) {
    gStop = true;
}

struct Options {
    int port = 7899;
    std::string dumpPng;
    std::string thumbPng;
    std::string iconPng;
    int iconSize = 256;
    std::vector<std::string> steps;
    bool dumpBusy = false;
    bool listButtons = false;
    double hoverX = -1, hoverY = -1;
    double scroll = 0;
    int lang = -1;  // -1 auto, 0 en, 1 zh
    bool show = false;  ///< open the dashboard on the panel once connected (the launcher)
    bool steamVrKeyboard = true;    ///< tapping a field opens the stock SteamVR keyboard (off: --no-steamvr-keyboard)
    std::string fakeKeyboardError;  ///< --dump-png: pretend SteamVR's keyboard failed with this
    std::string fakeClipboard;      ///< --dump-png: what "Paste" gets (never the real clipboard in previews)
};

void usage() {
    std::puts(
        "frameclash-panel " FRAMECLASH_PANEL_VERSION " - SteamVR dashboard panel for FrameClash\n"
        "usage: frameclash-panel [--show] [--api 127.0.0.1:PORT] [--lang en|zh] [--no-steamvr-keyboard]\n"
        "  --show: open the dashboard on the panel (a running instance is asked to do it)\n"
        "  --no-steamvr-keyboard: tapping a text field does not open the stock SteamVR keyboard (Paste only);\n"
        "             also FRAMECLASH_PANEL_KEYBOARD=off\n"
        "       frameclash-panel --dump-png OUT.png [--do STEP]... [--dump-busy] [--hover X,Y] [--scroll PX]\n"
        "                        [--list-buttons] [--thumb-png OUT.png]   (no SteamVR needed)\n"
        "       frameclash-panel --icon-png SIZE OUT.png\n"
        "steps: start | stop | refresh | test | select:GROUP=NODE | group:NAME | press:ID | click:X,Y | wait:MS\n"
        "       view:proxy|profiles | set:FIELD=TEXT | edit:FIELD | kbkeys:KEYS | kbdone[:TEXT] | kbclosed\n"
        "       (FIELD = url, name, path; dump options: --fake-clipboard TEXT --fake-keyboard-error MSG)\n"
        "The API address can also come from FRAMECLASH_UI_ADDR; it must be loopback.\n"
        "Exit codes: 0 SteamVR quit, 3 closed from the dashboard / another instance already running, 1 error.");
}

bool parsePort(const std::string& addr, int& port) {
    const size_t colon = addr.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = addr.substr(0, colon);
    if (host != "127.0.0.1" && host != "localhost" && host != "") return false;  // loopback only
    port = std::atoi(addr.c_str() + colon + 1);
    return port > 0 && port < 65536;
}

bool loadFonts(FontSet& fonts) {
    // The Frame (Arch: noto-fonts-cjk) and Debian/Ubuntu (fonts-noto-cjk) paths; FontSet falls back to fontconfig
    const char* dirs[] = {"/usr/share/fonts/noto-cjk/", "/usr/share/fonts/opentype/noto/"};
    for (const char* dir : dirs) {
        const std::string regular = std::string(dir) + "NotoSansCJK-Regular.ttc";
        if (::access(regular.c_str(), R_OK) == 0) {
            fonts.load(regular, std::string(dir) + "NotoSansCJK-Bold.ttc");
            return true;
        }
    }
    fonts.load("", "");
    return false;
}

std::string settingsUrl(int port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/";
}

/** Wait until the model has nothing queued or running (or the time is up). */
void waitIdle(Model& model, int maxMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int64_t until = nowMs() + maxMs;
    while (!model.idle() && nowMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

bool writePng(const std::string& path, int w, int h, const std::function<void(cairo_t*)>& paint) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surface);
    paint(cr);
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    if (!ok) std::fprintf(stderr, "can't write %s\n", path.c_str());
    return ok;
}

/** --dump-png: drive the real UI against the real frameclashd, then save the panel as a picture. */
int runDump(const Options& opt) {
    FontSet fonts;
    loadFonts(fonts);
    Model model(opt.port);
    std::vector<std::string> launched;
    KeyboardHooks hooks;
    if (opt.steamVrKeyboard)
        hooks.show = [&](const std::string& desc, const std::string& existing) -> std::string {
            std::printf("would call ShowKeyboardForOverlay(panel, Normal, SingleLine, Minimal, \"%s\", %zu existing bytes) + "
                        "SetKeyboardPositionForOverlay(panel, {0,0,0,0})\n",
                        desc.c_str(), existing.size());
            return opt.fakeKeyboardError;
        };
    hooks.hide = [] { std::printf("would call HideKeyboard()\n"); };
    hooks.paste = [&](std::string& error) -> std::string {
        if (opt.fakeClipboard.empty()) error = "clipboard empty";
        return opt.fakeClipboard;
    };
    PanelUi ui(
        model, fonts,
        [&] {
            const auto argv = detachedArgv(settingsArgv(settingsUrl(opt.port)), inPath("systemd-run"));
            std::string line;
            for (const auto& a : argv) line += (line.empty() ? "" : " ") + a;
            std::printf("would run: %s\n", line.c_str());
            launched.push_back(line);
        },
        hooks);
    if (opt.lang >= 0) ui.setLanguage(opt.lang == 1);
    cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kPanelWidth, kPanelHeight);
    auto redraw = [&] {
        cairo_t* cr = cairo_create(scratch);
        ui.draw(cr, model.snapshot());
        cairo_destroy(cr);
    };
    waitIdle(model, 20000);
    redraw();
    for (size_t i = 0; i < opt.steps.size(); ++i) {
        const std::string& step = opt.steps[i];
        const bool last = i + 1 == opt.steps.size();
        bool ok = true;
        if (step == "start" || step == "stop" || step == "test") {
            ok = ui.press(step == "test" ? step : "toggle");
        } else if (step.rfind("press:", 0) == 0) {
            ok = ui.press(step.substr(6));
        } else if (step.rfind("group:", 0) == 0) {
            ok = ui.showGroup(step.substr(6));
        } else if (step.rfind("select:", 0) == 0) {
            const std::string arg = step.substr(7);
            const size_t eq = arg.find('=');
            ok = eq != std::string::npos && ui.showGroup(arg.substr(0, eq));
            if (ok) {
                redraw();
                ok = ui.press("node:" + arg.substr(eq + 1));
            }
        } else if (step.rfind("click:", 0) == 0) {
            double x = 0, y = 0;
            ok = std::sscanf(step.c_str() + 6, "%lf,%lf", &x, &y) == 2;
            if (ok) {
                ui.pointerDown(x, y);
                ui.pointerUp(x, y);
            }
        } else if (step.rfind("view:", 0) == 0) {  // view:overview, view:proxies, view:profiles
            ok = ui.press(step);
        } else if (step.rfind("set:", 0) == 0) {
            const size_t eq = step.find('=');
            ok = eq != std::string::npos;
            if (ok) ui.setField(step.substr(4, eq - 4), step.substr(eq + 1));
        } else if (step.rfind("edit:", 0) == 0) {
            ok = ui.startEdit(step.substr(5));
        } else if (step == "kbdone" || step.rfind("kbdone:", 0) == 0) {
            ok = ui.keyboardDone(step.size() > 7 ? step.substr(7) : "");
        } else if (step.rfind("kbkeys:", 0) == 0) {  // one key per char; "<" = backspace, "|" = enter
            for (char c : step.substr(7)) ok = ui.keyboardChar(c == '<' ? "\b" : c == '|' ? "\n" : std::string(1, c));
        } else if (step == "kbclosed") {
            ok = ui.keyboardClosed();
        } else if (step.rfind("wait:", 0) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(step.c_str() + 5)));
        } else {
            std::fprintf(stderr, "unknown step: %s\n", step.c_str());
            return 2;
        }
        if (!ok) std::fprintf(stderr, "step \"%s\": no such enabled button now\n", step.c_str());
        if (last && opt.dumpBusy) {
            // capture while the action is still running (e.g. "refreshing")
            const int64_t until = nowMs() + 3000;
            while (model.snapshot().busy == Busy::None && nowMs() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            waitIdle(model, 60000);
        }
        redraw();
    }
    if (opt.scroll != 0) {
        ui.scroll(opt.scroll);
        redraw();
    }
    if (opt.hoverX >= 0) {
        ui.pointerMove(opt.hoverX, opt.hoverY);
    }
    const Snapshot snap = model.snapshot();
    const bool ok = writePng(opt.dumpPng, kPanelWidth, kPanelHeight, [&](cairo_t* cr) { ui.draw(cr, snap); });
    if (opt.listButtons)
        for (const Hit& h : ui.hits())
            std::printf("%-40s %4.0f,%-4.0f %4.0fx%-3.0f %s\n", h.id.c_str(), h.x, h.y, h.w, h.h,
                        h.enabled ? "enabled" : "disabled");
    if (!opt.thumbPng.empty())
        writePng(opt.thumbPng, kThumbnailSize, kThumbnailSize, [&](cairo_t* cr) { ui.drawThumbnail(cr, snap); });
    cairo_surface_destroy(scratch);
    std::printf("state: reach=%s running=%d groups=%zu busy=%d message=\"%s\"\n",
                snap.reach == Reach::Ok ? "ok" : snap.reach == Reach::Unreachable ? "unreachable" : "unknown",
                snap.status.running ? 1 : 0, snap.groups.size(), static_cast<int>(snap.busy), snap.message.c_str());
    return ok ? 0 : 1;
}

/** The app icon (for the .desktop launcher / SteamVR "Launch program"): the dashboard thumbnail without the status
 *  dot. pkg/icons/frameclash.png is `frameclash-panel --icon-png 256` (built as in tools/panel-preview). */
int runIcon(const Options& opt) {
    FontSet fonts;
    loadFonts(fonts);
    return writePng(opt.iconPng, opt.iconSize, opt.iconSize, [&](cairo_t* cr) {
               const double s = opt.iconSize / static_cast<double>(kThumbnailSize);
               cairo_scale(cr, s, s);
               const Pen pen {cr, &fonts};
               pen.color(cCard);
               pen.roundedRect(0, 0, kThumbnailSize, kThumbnailSize, 48);
               cairo_fill(cr);
               const double tw = pen.measure("FC", 118, true);
               pen.text((kThumbnailSize - tw) / 2, 168, "FC", 118, cText, true);
               pen.color(cAmber);
               cairo_rectangle(cr, 48, 200, kThumbnailSize - 96, 10);
               cairo_fill(cr);
           })
               ? 0
               : 1;
}

/** Single instance: listen on an abstract socket; a second launch asks the first to show the panel. */
int claimInstance(bool sendShow, bool& otherRunning) {
    otherRunning = false;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    const size_t n = std::strlen(kInstanceSocket);
    std::memcpy(addr.sun_path + 1, kInstanceSocket, n);  // sun_path[0] = 0: abstract (nothing on disk)
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0 && ::listen(fd, 4) == 0) return fd;
    ::close(fd);
    otherRunning = true;
    const int c = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sendShow && c >= 0 && ::connect(c, reinterpret_cast<sockaddr*>(&addr), len) == 0) {
        ssize_t ignored = ::write(c, "show\n", 5);
        (void)ignored;
    }
    if (c >= 0) ::close(c);
    return -1;
}

bool showRequested(int listenFd) {
    bool show = false;
    for (;;) {
        const int c = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) break;
        char buf[16] = {};
        struct timeval tv {0, 200000};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::read(c, buf, sizeof(buf) - 1) > 0 && std::strncmp(buf, "show", 4) == 0) show = true;
        ::close(c);
    }
    return show;
}

int runVr(const Options& opt) {
    bool otherRunning = false;
    const int instanceFd = claimInstance(opt.show, otherRunning);
    if (otherRunning) {
        std::fprintf(stderr, "frameclash-panel is already running: asked it to show the panel\n");
        return kExitClosed;
    }
    FontSet fonts;
    loadFonts(fonts);
    Model model(opt.port);
    model.setInterval(10000);
    VrOverlay vr;
    KeyboardHooks hooks;
    if (opt.steamVrKeyboard)
        hooks.show = [&](const std::string& desc, const std::string& existing) -> std::string {
            return vr.showKeyboard(desc, existing, 4096);
        };
    hooks.hide = [&] { vr.hideKeyboard(); };
    hooks.paste = [](std::string& error) { return readClipboard(error); };
    PanelUi ui(
        model, fonts,
        [&] {
            std::string error;
            if (!spawnDetached(settingsArgv(settingsUrl(opt.port)), error))
                std::fprintf(stderr, "can't open the settings page: %s\n", error.c_str());
        },
        hooks);
    ui.setLanguage(opt.lang == 1);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kPanelWidth, kPanelHeight);
    cairo_surface_t* thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kThumbnailSize, kThumbnailSize);
    std::vector<uint8_t> rgba, thumbRgba;
    int exitCode = 0;
    int64_t nextConnect = 0;
    std::string lastConnectMessage;
    bool showPending = opt.show;
    uint64_t drawnSeq = ~0ull;
    bool dirty = true;
    bool wasVisible = false;
    Reach thumbReach = Reach::Unknown;
    bool thumbRunning = false;
    bool thumbSent = false;
    while (!gStop) {
        if (instanceFd >= 0 && showRequested(instanceFd)) showPending = true;
        if (!vr.connected()) {
            if (nowMs() < nextConnect) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            std::string message;
            const auto result = vr.connect(kPanelWidth, kPanelHeight, message);
            if (result != VrOverlay::ConnectResult::Ok) {
                if (message != lastConnectMessage) std::fprintf(stderr, "[VR] not connected: %s\n", message.c_str());
                lastConnectMessage = message;
                nextConnect = nowMs() + kRetryConnectMs;
                continue;
            }
            std::fprintf(stderr, "[VR] connected\n");
            lastConnectMessage.clear();
            dirty = true;
            thumbSent = false;
        }
        const VrEvents events = vr.pollEvents();
        if (events.quit || !vr.steamVrAlive()) {
            exitCode = 0;
            break;
        }
        if (events.closeRequested) {
            exitCode = kExitClosed;
            break;
        }
        for (const PointerInput& p : events.pointer) {
            switch (p.type) {
                case PointerInput::Type::Move: dirty |= ui.pointerMove(p.x, p.y); break;
                case PointerInput::Type::Down: dirty |= ui.pointerDown(p.x, p.y); break;
                case PointerInput::Type::Up: dirty |= ui.pointerUp(p.x, p.y); break;
                case PointerInput::Type::Leave: dirty |= ui.pointerLeave(); break;
                case PointerInput::Type::Scroll: dirty |= ui.scroll(p.y); break;
            }
        }
        for (const KeyboardInput& k : events.keyboard) {
            switch (k.type) {
                case KeyboardInput::Type::Char: dirty |= ui.keyboardChar(k.text); break;
                case KeyboardInput::Type::Done: dirty |= ui.keyboardDone(k.text); break;
                case KeyboardInput::Type::Closed: dirty |= ui.keyboardClosed(); break;
            }
        }
        const bool visible = vr.panelVisible();
        if (visible != wasVisible) {
            model.setInterval(visible ? 2000 : 10000);
            vr.setPanelScroll(visible);
            wasVisible = visible;
            dirty = true;
        }
        const Snapshot snap = model.snapshot();
        if (snap.seq != drawnSeq) dirty = true;
        if (dirty && (visible || showPending || drawnSeq == ~0ull)) {
            cairo_t* cr = cairo_create(surface);
            ui.draw(cr, snap);
            cairo_destroy(cr);
            surfaceToRgba(surface, rgba);
            vr.submitPanel(rgba.data());
            drawnSeq = snap.seq;
            dirty = false;
        }
        if (!thumbSent || snap.reach != thumbReach || snap.status.running != thumbRunning) {
            cairo_t* cr = cairo_create(thumb);
            ui.drawThumbnail(cr, snap);
            cairo_destroy(cr);
            surfaceToRgba(thumb, thumbRgba);
            thumbSent = vr.submitThumbnail(thumbRgba.data(), kThumbnailSize);
            thumbReach = snap.reach;
            thumbRunning = snap.status.running;
        }
        if (showPending) {
            vr.showPanel();  // an image was submitted above, so it never opens blank
            showPending = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(visible ? 33 : 250));
    }
    vr.shutdown();
    cairo_surface_destroy(thumb);
    cairo_surface_destroy(surface);
    if (instanceFd >= 0) ::close(instanceFd);
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (const char* env = std::getenv("FRAMECLASH_UI_ADDR"); env != nullptr && *env != 0) {
        if (!parsePort(env, opt.port)) {
            std::fprintf(stderr, "FRAMECLASH_UI_ADDR must be 127.0.0.1:PORT\n");
            return 1;
        }
    }
    if (const char* lang = std::getenv("LANG"); lang != nullptr && std::strncmp(lang, "zh", 2) == 0) opt.lang = 1;
    if (const char* kb = std::getenv("FRAMECLASH_PANEL_KEYBOARD"); kb != nullptr && std::strcmp(kb, "off") == 0)
        opt.steamVrKeyboard = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--version") {
            std::puts("frameclash-panel " FRAMECLASH_PANEL_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--api") {
            if (!parsePort(next(), opt.port)) {
                std::fprintf(stderr, "--api must be 127.0.0.1:PORT (loopback only)\n");
                return 2;
            }
        } else if (a == "--lang") {
            const std::string l = next();
            opt.lang = l == "zh" ? 1 : 0;
        } else if (a == "--dump-png") {
            opt.dumpPng = next();
        } else if (a == "--thumb-png") {
            opt.thumbPng = next();
        } else if (a == "--icon-png") {
            opt.iconSize = std::atoi(next().c_str());
            opt.iconPng = next();
        } else if (a == "--do") {
            opt.steps.push_back(next());
        } else if (a == "--dump-busy") {
            opt.dumpBusy = true;
        } else if (a == "--list-buttons") {
            opt.listButtons = true;
        } else if (a == "--hover") {
            std::sscanf(next().c_str(), "%lf,%lf", &opt.hoverX, &opt.hoverY);
        } else if (a == "--scroll") {
            opt.scroll = std::atof(next().c_str());
        } else if (a == "--show") {
            opt.show = true;
        } else if (a == "--no-steamvr-keyboard") {
            opt.steamVrKeyboard = false;
        } else if (a == "--fake-keyboard-error") {
            opt.fakeKeyboardError = next();
        } else if (a == "--fake-clipboard") {
            opt.fakeClipboard = next();
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (!opt.iconPng.empty()) return runIcon(opt);
    if (!opt.dumpPng.empty()) return runDump(opt);
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    return runVr(opt);
}
