#!/usr/bin/env bash
# FrameClash uninstaller. No sudo. Stops with SIGTERM (systemd stop), never SIGKILL.
#   uninstall.sh           remove program, `frameclash` command, VR panel, units, launchers, icons, runtime data,
#                          proxy env file;
#                          KEEP ~/.config/frameclash
#   uninstall.sh --purge   also delete ~/.config/frameclash (profiles, subscription URL, secret)
set -euo pipefail
if [[ "${FRAMECLASH_NO_SYSTEMCTL:-}" == 1 ]]; then systemctl() { echo "(skipped) systemctl $*"; }; fi
purge=false
for a in "$@"; do case "$a" in --purge) purge=true ;; *) echo "unknown option $a" >&2; exit 2 ;; esac; done
cfg="${XDG_CONFIG_HOME:-$HOME/.config}"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
lib="$HOME/.local/lib/frameclash"
envd="$cfg/environment.d/90-frameclash-proxy.conf"

# 1. proxy env first, so nothing is left pointing at a dead proxy
if [[ -f "$envd" ]] && grep -q FrameClash "$envd"; then rm -f "$envd"; echo "removed $envd"; fi
systemctl --user unset-environment http_proxy https_proxy all_proxy no_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY 2>/dev/null || true
# 1b. TUN: undo it while the daemon still runs (tun off -> SIGTERM cleanup), then the root-owned helper; that
#     needs sudo, so only when run in a terminal. Otherwise the note at the end says how.
tun_rule() { pkcheck --action-id org.freedesktop.resolve1.set-domains --process $$ >/dev/null 2>&1; }  # rules dir is 0700
if [[ -e "$(dirname "$HOME")/.frameclash" ]] || tun_rule; then has_tun=true; else has_tun=false; fi
if $has_tun && [[ -t 0 && -x "$lib/tun-setup.sh" ]]; then
  echo "Removing TUN (asks for your sudo password)..."
  "$lib/tun-setup.sh" remove || echo "WARNING: TUN removal failed; see the note below" >&2
fi
# 2. stop + disable (SIGTERM via the units' KillSignal; frameclashd waits for mihomo to exit).
#    The panel first: disabling it also removes its steamvr.service.wants/ link.
systemctl --user disable --now frameclash-panel.service 2>/dev/null || true
if pgrep -u "$(id -u)" -f "$lib/frameclash-panel" >/dev/null 2>&1; then
  pkill -TERM -u "$(id -u)" -f "$lib/frameclash-panel" || true   # e.g. started by hand / from the launcher
  for _ in $(seq 10); do pgrep -u "$(id -u)" -f "$lib/frameclash-panel" >/dev/null || break; sleep 1; done
fi
systemctl --user disable --now frameclash.service 2>/dev/null || true
if pgrep -u "$(id -u)" -f "$lib/mihomo" >/dev/null 2>&1; then
  echo "mihomo still running; sending SIGTERM once and waiting up to 20s"
  pkill -TERM -u "$(id -u)" -f "$lib/mihomo" || true
  for _ in $(seq 20); do pgrep -u "$(id -u)" -f "$lib/mihomo" >/dev/null || break; sleep 1; done
  pgrep -u "$(id -u)" -f "$lib/mihomo" >/dev/null && echo "WARNING: mihomo still running (not force-killed). Check: pgrep -af mihomo" >&2
fi
# 3. files (the `frameclash` command only if it is our symlink)
cli="$HOME/.local/bin/frameclash"
if [[ -L "$cli" && "$(readlink "$cli")" == "$lib/frameclashd" ]]; then rm -f "$cli"; fi
rm -f "$cfg/systemd/user/frameclash.service" "$data/applications/frameclash.desktop" \
      "$cfg/systemd/user/frameclash-panel.service" "$cfg/systemd/user/steamvr.service.wants/frameclash-panel.service" \
      "$data/applications/frameclash-panel.desktop"
for size in 48 128 256; do rm -f "$data/icons/hicolor/${size}x${size}/apps/frameclash-panel.png"; done
rm -f "$data/icons/hicolor/256x256/apps/frameclash.png"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$data/applications" 2>/dev/null || true
systemctl --user daemon-reload 2>/dev/null || true
systemctl --user reset-failed frameclash.service frameclash-panel.service 2>/dev/null || true
rm -rf "$data/frameclash"
created=$( [[ -f "$lib/.created-dirs" ]] && cat "$lib/.created-dirs" || true )
created_files=$( [[ -f "$lib/.created-files" ]] && cat "$lib/.created-files" || true )
rm -rf "$lib"   # this script may live here; bash keeps the open file, so this is safe
# mimeinfo.cache written by update-desktop-database at install: remove only if install created it and no
# other .desktop files are left next to it
while IFS= read -r f; do
  [[ -n "$f" && -f "$f" && "$(basename "$f")" == mimeinfo.cache ]] || continue
  compgen -G "$(dirname "$f")/*.desktop" >/dev/null || rm -f "$f"
done <<< "$created_files"
if $purge; then rm -rf "$cfg/frameclash"; fi
# remove generic XDG dirs only if install.sh created them and they are now empty
# deepest first (e.g. icons/hicolor/48x48/apps before icons/hicolor/48x48)
while IFS= read -r d; do [[ -n "$d" && -d "$d" ]] && rmdir --ignore-fail-on-non-empty "$d" 2>/dev/null || true
done < <(printf '%s\n' "$created" | sort -u | awk 'NF { print length($0) "\t" $0 }' | sort -rn | cut -f2-)
tun_dir="$(dirname "$HOME")/.frameclash"
if [[ -e "$tun_dir" ]] || tun_rule; then
  echo "NOTE: TUN's root-owned parts are still there. Remove them with:"
  echo "      sudo setcap -r $tun_dir/mihomo; sudo rm -rf $tun_dir; sudo rm -f /etc/polkit-1/rules.d/49-frameclash-resolved.rules"
fi
if $purge; then
  echo "Removed FrameClash including settings (~/.config/frameclash)."
else
  echo "Removed FrameClash. Settings kept in $cfg/frameclash (run again with --purge, or rm -rf it)."
fi
