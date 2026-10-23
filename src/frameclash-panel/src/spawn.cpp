// Adapted from frameeyeosc's panel/src/setup_tools.cpp (MIT, Copyright (c) 2026 konsti219; Copyright (c) 2026
// sasaken1102r). See spawn.h.
#include "spawn.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

bool inPath(const std::string& name) {
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    const std::string dirs = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t from = 0;
    while (from <= dirs.size()) {
        const size_t colon = std::min(dirs.find(':', from), dirs.size());
        const std::string dir = dirs.substr(from, colon - from);
        if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        from = colon + 1;
    }
    return false;
}

std::vector<std::string> settingsArgv(const std::string& url) {
    // url is ours (http://127.0.0.1:<port>/), never user text, so it is safe inside the sh -c string
    return {"sh", "-c", "flatpak run org.chromium.Chromium '" + url + "' || xdg-open '" + url + "'"};
}

std::vector<std::string> detachedArgv(const std::vector<std::string>& argv, bool systemdRun) {
    if (!systemdRun) return argv;
    std::vector<std::string> out = {"systemd-run", "--user", "--collect", "--quiet"};
    for (const char* name : kSpawnEnvironment) {
        if (std::getenv(name) != nullptr) {
            out.push_back("-E");
            out.push_back(name);
        }
    }
    out.push_back("--");
    out.insert(out.end(), argv.begin(), argv.end());
    return out;
}

bool spawnDetached(const std::vector<std::string>& program, std::string& error) {
    if (program.empty()) {
        error = "nothing to run";
        return false;
    }
    const std::vector<std::string> argv = detachedArgv(program, inPath("systemd-run"));
    std::vector<char*> args;
    for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        ::setsid();
        const pid_t grandchild = ::fork();
        if (grandchild != 0) ::_exit(grandchild < 0 ? 1 : 0);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, 0);
            ::dup2(null, 1);
            ::dup2(null, 2);
            if (null > 2) ::close(null);
        }
        // Nothing of the panel's (sockets, the GPU) goes along
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
            for (int fd = 3; fd < 4096; ++fd) ::close(fd);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "fork failed in the child";
        return false;
    }
    return true;
}

// ---- 0.2.1: clipboard (FrameClash code, MIT) ----
// Read in-process from the X server: the Frame ships no wl-paste/xclip/xsel, and gamescope keeps the clipboard on
// its Xwayland (:0 on the Frame). libxcb, unlike Xlib, never exit()s on a connection error.
namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

xcb_atom_t atom(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_reply_t* r =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
    const xcb_atom_t a = r != nullptr ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    std::free(r);
    return a;
}

// DISPLAY first, then every local X socket (a systemd user unit usually has no DISPLAY)
std::vector<std::string> xDisplays() {
    std::vector<std::string> out;
    if (const char* d = std::getenv("DISPLAY"); d != nullptr && *d != 0) out.push_back(d);
    for (int n = 0; n < 4; ++n) {
        struct stat st {};
        const std::string name = ":" + std::to_string(n);
        if (::stat(("/tmp/.X11-unix/X" + std::to_string(n)).c_str(), &st) == 0 && S_ISSOCK(st.st_mode) &&
            std::find(out.begin(), out.end(), name) == out.end())
            out.push_back(name);
    }
    return out;
}

// CLIPBOARD as UTF8_STRING from one display. false = no owner there (try the next display).
bool readFrom(const std::string& display, std::string& out, std::string& error) {
    xcb_connection_t* c = xcb_connect(display.c_str(), nullptr);
    if (xcb_connection_has_error(c)) {
        xcb_disconnect(c);
        return false;
    }
    const xcb_atom_t clip = atom(c, "CLIPBOARD"), utf8 = atom(c, "UTF8_STRING"), incr = atom(c, "INCR"),
                     prop = atom(c, "FRAMECLASH_CLIPBOARD");
    xcb_get_selection_owner_reply_t* owner =
        xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, clip), nullptr);
    const bool owned = owner != nullptr && owner->owner != XCB_NONE;
    std::free(owner);
    if (!owned) {
        xcb_disconnect(c);
        return false;
    }
    const xcb_window_t win = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, xcb_setup_roots_iterator(xcb_get_setup(c)).data->root, 0, 0, 1,
                      1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
    xcb_convert_selection(c, win, clip, utf8, prop, XCB_CURRENT_TIME);
    xcb_flush(c);
    error = "the app that copied did not answer";
    const int64_t until = nowMs() + 1500;
    for (bool done = false; !done && nowMs() < until && !xcb_connection_has_error(c);) {
        pollfd p {xcb_get_file_descriptor(c), POLLIN, 0};
        ::poll(&p, 1, static_cast<int>(std::max<int64_t>(1, until - nowMs())));
        while (xcb_generic_event_t* ev = xcb_poll_for_event(c)) {
            if ((ev->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
                done = true;
                if (reinterpret_cast<xcb_selection_notify_event_t*>(ev)->property == XCB_NONE) {
                    error = "the clipboard holds no text";
                } else {
                    xcb_get_property_reply_t* r = xcb_get_property_reply(
                        c, xcb_get_property(c, 1, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 65536 / 4), nullptr);
                    if (r == nullptr || r->type == incr || r->bytes_after > 0) {
                        error = "clipboard text too large (max 64 KB)";
                    } else {
                        out.assign(static_cast<const char*>(xcb_get_property_value(r)),
                                   static_cast<size_t>(xcb_get_property_value_length(r)));
                        error.clear();
                    }
                    std::free(r);
                }
            }
            std::free(ev);
        }
    }
    xcb_disconnect(c);
    return true;
}

}  // namespace

std::string readClipboard(std::string& error) {
    const auto displays = xDisplays();
    if (displays.empty()) {
        error = "no X display found";
        return "";
    }
    for (const auto& d : displays) {
        std::string out;
        if (!readFrom(d, out, error)) continue;
        // only the display and the size are logged, never the text (it may be a subscription URL)
        std::fprintf(stderr, "[clipboard] %s -> %zu bytes%s%s\n", d.c_str(), out.size(), error.empty() ? "" : ", ",
                     error.c_str());
        return out;
    }
    error = "clipboard empty";
    return "";
}
