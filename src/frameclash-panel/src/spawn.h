// Starting a program (Chromium with the full settings page) outside the panel's own service.
// Adapted from frameeyeosc's panel/src/setup_tools.{h,cpp} (spawnDetached / detachedArgv / kSpawnEnvironment;
// https://github.com/sasaken1102r/frameeyeosc, commit c4294a9).
// Copyright (c) 2026 konsti219; Copyright (c) 2026 sasaken1102r (original). MIT License:
// third_party_frameeyeosc/LICENSE.frameeyeosc. Changes for FrameClash are under the same license.
#pragma once

#include <string>
#include <vector>

/** What a program started through systemd-run takes along from the panel's environment (only those set). */
constexpr const char* kSpawnEnvironment[] = {"DISPLAY",         "WAYLAND_DISPLAY", "XAUTHORITY",
                                             "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
                                             "DBUS_SESSION_BUS_ADDRESS", "LANG", "LC_ALL"};

/** The settings page in Chromium (flatpak), falling back to xdg-open. */
std::vector<std::string> settingsArgv(const std::string& url);

/** Through "systemd-run --user --collect --quiet -E ..." when available (so it outlives a panel restart). */
std::vector<std::string> detachedArgv(const std::vector<std::string>& argv, bool systemdRun);

bool inPath(const std::string& name);

/** Start it detached (double fork, own session, stdio to /dev/null). */
bool spawnDetached(const std::vector<std::string>& program, std::string& error);

/** Clipboard text for "Paste" (0.2.1): the X CLIPBOARD selection read with libxcb ($DISPLAY, then each local X
 *  socket; gamescope's Xwayland holds it on the Frame), 1.5 s timeout, at most 64 KB. The text is never logged. @return "" with `error` set when there is nothing to paste. */
std::string readClipboard(std::string& error);
