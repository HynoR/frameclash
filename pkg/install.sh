#!/usr/bin/env bash
# FrameClash installer — run ON THE STEAM FRAME as user steamos. Never uses sudo; writes only under $HOME.
#   ./install.sh            install or update, enable + start frameclash.service, and install the SteamVR
#                           dashboard panel (frameclash-panel.service, starts with SteamVR)
#   ./install.sh --no-panel the same without the panel (an installed panel is removed)
# Test on another machine with a scratch HOME:
#   FRAMECLASH_NO_SYSTEMCTL=1 FRAMECLASH_ALLOW_ANY_ARCH=1 HOME=/tmp/x ./install.sh
set -euo pipefail
if [[ "${FRAMECLASH_NO_SYSTEMCTL:-}" == 1 ]]; then systemctl() { echo "(skipped) systemctl $*"; }; fi
here="$(cd "$(dirname "$0")" && pwd)"
with_panel=true
for a in "$@"; do
  case "$a" in
    --no-panel) with_panel=false ;;
    *) echo "unknown option $a (use --no-panel)" >&2; exit 2 ;;
  esac
done
[[ -x "$here/bin/frameclash-panel" || -d "$here/panel-src" ]] || with_panel=false
[[ "$(id -u)" != 0 ]] || { echo "Do not run as root / with sudo. Run as your normal user (steamos)." >&2; exit 1; }
if [[ "$(uname -m)" != aarch64 && "${FRAMECLASH_ALLOW_ANY_ARCH:-}" != 1 ]]; then
  echo "This package is for the Steam Frame (aarch64); this machine is $(uname -m)." >&2; exit 1
fi
cfg="${XDG_CONFIG_HOME:-$HOME/.config}"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
lib="$HOME/.local/lib/frameclash"
unit_dir="$cfg/systemd/user"
conf="$cfg/frameclash"
run="$data/frameclash/mihomo"
icons="$data/icons/hicolor"
panel_desktop="$data/applications/frameclash-panel.desktop"
bin_dir="$HOME/.local/bin"
cli="$bin_dir/frameclash"

# The VR panel is built here first (like frameeyeosc; SteamOS ships cmake, ninja, g++ and the -dev files), so a
# failed build leaves nothing half-installed. It links SteamVR's own libopenvr_api.so.
if $with_panel && [[ ! -x "$here/bin/frameclash-panel" ]]; then
  echo "==> Building the VR panel (about a minute)"
  cmake -G Ninja -S "$here/panel-src" -B "$here/panel-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C "$here/panel-build" >/dev/null
  "$here/panel-build/frameclash-panel-tests" >/dev/null 2>&1 || { echo "panel tests failed" >&2; exit 1; }
  strip -o "$here/bin/frameclash-panel" "$here/panel-build/frameclash-panel"
fi
echo "==> Installing FrameClash $(cat "$here/VERSION") into $lib"
if [[ -f "$unit_dir/frameclash.service" ]]; then
  echo "    existing install found: stopping it first (SIGTERM)"
  systemctl --user stop frameclash.service || true
fi
# remember which generic XDG dirs did not exist before, so uninstall can remove them again (only if empty).
# Appended on every install (an update may create new ones, e.g. the panel's icon dirs).
created=()
generic=("$HOME/.local" "$bin_dir" "$data" "$cfg" "$HOME/.local/lib" "$unit_dir" "$cfg/systemd" "$data/applications" "$cfg/environment.d"
         "$unit_dir/steamvr.service.wants" "$data/icons" "$icons")
for size in 48 128 256; do generic+=("$icons/${size}x${size}" "$icons/${size}x${size}/apps"); done
for d in "${generic[@]}"; do
  [[ -d "$d" ]] || created+=("$d")
done
mkdir -p "$lib" "$unit_dir" "$conf/profiles" "$run" "$data/applications"
if ((${#created[@]})); then printf '%s\n' "${created[@]}" >> "$lib/.created-dirs"; else touch "$lib/.created-dirs"; fi
chmod 700 "$conf" "$conf/profiles"
for f in frameclashd mihomo; do
  install -m755 "$here/bin/$f" "$lib/.$f.new" && mv -f "$lib/.$f.new" "$lib/$f"
done
install -m755 "$here/proxy-env.sh" "$lib/proxy-env.sh"
install -m755 "$here/uninstall.sh" "$lib/uninstall.sh"
install -m755 "$here/tun-setup.sh" "$lib/tun-setup.sh"
install -m644 "$here/README.md" "$lib/README.md"
install -m644 "$here/VERSION" "$lib/VERSION"
# geo data shipped so mihomo does not need to download it on first start
for g in Country.mmdb GeoSite.dat; do
  [[ -f "$run/$g" ]] || install -m644 "$here/geo/$g" "$run/$g"
done
[[ -f "$conf/env" ]] || install -m600 "$here/env.example" "$conf/env"
install -m644 "$here/frameclash.service" "$unit_dir/frameclash.service"
# the panel launcher's icon, by absolute path in its .desktop
icon="$icons/256x256/apps/frameclash.png"
install -Dm644 "$here/icons/frameclash.png" "$icon"
rm -f "$data/applications/frameclash.desktop"  # the old "FrameClash Web" launcher (Chromium is not preinstalled on the Frame)
install -m644 "$here/LICENSE" "$lib/LICENSE"
# the `frameclash` command (status/start/stop/import-url/...): a symlink to frameclashd, which acts as the CLI
# when called by that name. Never replaces a file that is not ours.
mkdir -p "$bin_dir"
if [[ -e "$cli" || -L "$cli" ]] && [[ "$(readlink "$cli" 2>/dev/null)" != "$lib/frameclashd" ]]; then
  echo "    WARNING: $cli exists and is not FrameClash's; left alone. Use: $lib/frameclashd cli ..." >&2
else
  ln -sfn "$lib/frameclashd" "$cli"
fi
install -m644 "$here/THIRD_PARTY_NOTICES.md" "$lib/THIRD_PARTY_NOTICES.md"

# SteamVR dashboard panel (systemd user unit tied to steamvr.service, like frameeyeosc; no SteamVR manifest)
if $with_panel; then
  echo "==> Installing the SteamVR dashboard panel"
  # replace the binary by rename: a running panel keeps its old file until it restarts
  install -m755 "$here/bin/frameclash-panel" "$lib/.frameclash-panel.new" && mv -f "$lib/.frameclash-panel.new" "$lib/frameclash-panel"
  install -m644 "$here/frameclash-panel.service" "$unit_dir/frameclash-panel.service"
  for size in 48 128 256; do rm -f "$icons/${size}x${size}/apps/frameclash-panel.png"; done  # 0.2.x icons
  # Exec needs the absolute path (Steam may start it without ~/.local/lib/frameclash in PATH)
  sed -e "s|@BINARY@|$lib/frameclash-panel|" -e "s|@ICON@|$icon|" "$here/frameclash-panel.desktop" > "$panel_desktop.tmp"
  chmod 644 "$panel_desktop.tmp" && mv -f "$panel_desktop.tmp" "$panel_desktop"
elif [[ -f "$unit_dir/frameclash-panel.service" || -e "$lib/frameclash-panel" ]]; then
  echo "==> --no-panel: removing the installed panel"
  systemctl --user disable --now frameclash-panel.service 2>/dev/null || true
  rm -f "$unit_dir/frameclash-panel.service" "$lib/frameclash-panel" "$lib/libopenvr_api.so" "$panel_desktop"
  for size in 48 128 256; do rm -f "$icons/${size}x${size}/apps/frameclash-panel.png"; done
fi
if command -v update-desktop-database >/dev/null 2>&1; then
  # it writes mimeinfo.cache; remember if that was us, so uninstall can remove it again
  [[ -e "$data/applications/mimeinfo.cache" ]] || echo "$data/applications/mimeinfo.cache" >> "$lib/.created-files"
  update-desktop-database "$data/applications" 2>/dev/null || true
fi

systemctl --user daemon-reload
systemctl --user enable --now frameclash.service
if $with_panel; then
  systemctl --user enable frameclash-panel.service   # -> steamvr.service.wants/
  # start (or restart on the new binary) now only if SteamVR is running; otherwise it starts with SteamVR
  if systemctl --user is-active --quiet steamvr.service 2>/dev/null; then
    # one started from the app list runs outside the unit and would keep the old binary: SIGTERM it first
    systemctl --user stop frameclash-panel.service 2>/dev/null || true
    pkill -TERM -u "$(id -u)" -f "$lib/frameclash-panel" 2>/dev/null || true
    for _ in $(seq 10); do pgrep -u "$(id -u)" -f "$lib/frameclash-panel" >/dev/null || break; sleep 1; done
    systemctl --user restart frameclash-panel.service || true
  else
    echo "    steamvr.service is not active: the panel will start with SteamVR"
  fi
fi
sleep 1
systemctl --user --no-pager --lines=0 status frameclash.service || true
cat <<MSG

==> Done. Open the control page:  http://127.0.0.1:7899/
    (or "FrameClash" in the app list; via SSH:  ssh -L 7899:127.0.0.1:7899 steamos@<frame-ip>)
    Then import a subscription URL or a local YAML, and press Start.
    In VR: SteamVR dashboard -> "FrameClash" icon (status, start/stop, nodes, latency test, Profiles: import by Paste).
    Command line (also over SSH):  $cli status | import-url URL | import-file PATH | profiles | use NAME | --help
      e.g. from the Mac:  ssh steamos@<frame-ip> ~/.local/bin/frameclash status
    Proxy for apps: http://127.0.0.1:7890 (HTTP+SOCKS5).  Optional env vars: $lib/proxy-env.sh on
    Uninstall: $lib/uninstall.sh [--purge]
MSG
