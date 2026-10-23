#!/usr/bin/env bash
# Optional: point per-user programs at FrameClash via environment variables. No root needed.
#   proxy-env.sh on      write ~/.config/environment.d/90-frameclash-proxy.conf + set it in the running user manager
#   proxy-env.sh off     remove both (do this BEFORE stopping/uninstalling FrameClash, or apps lose network)
#   proxy-env.sh status
# Only affects programs started AFTER this (fully effective after a reboot / re-login), and only those that
# honour http_proxy/https_proxy/all_proxy (curl, wget, git, pip, many CLI tools, some Chromium/Electron apps).
set -euo pipefail
port="${FRAMECLASH_MIXED_PORT:-7890}"
f="${XDG_CONFIG_HOME:-$HOME/.config}/environment.d/90-frameclash-proxy.conf"
vars=(http_proxy https_proxy all_proxy no_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY)
np="localhost,127.0.0.1,::1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,.local"
case "${1:-status}" in
  on)
    mkdir -p "$(dirname "$f")"
    {
      echo "# written by FrameClash proxy-env.sh; remove with: ~/.local/lib/frameclash/proxy-env.sh off"
      for v in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY; do echo "$v=http://127.0.0.1:$port"; done
      for v in all_proxy ALL_PROXY; do echo "$v=socks5h://127.0.0.1:$port"; done
      for v in no_proxy NO_PROXY; do echo "$v=$np"; done
    } > "$f"
    systemctl --user set-environment \
      "http_proxy=http://127.0.0.1:$port" "https_proxy=http://127.0.0.1:$port" \
      "HTTP_PROXY=http://127.0.0.1:$port" "HTTPS_PROXY=http://127.0.0.1:$port" \
      "all_proxy=socks5h://127.0.0.1:$port" "ALL_PROXY=socks5h://127.0.0.1:$port" \
      "no_proxy=$np" "NO_PROXY=$np" || true
    echo "Proxy env ON ($f). New user services pick it up now; reboot for the whole session (Steam etc.)."
    echo "WARNING: while this is on, programs that use it lose network if FrameClash is stopped."
    ;;
  off)
    if [[ -f "$f" ]] && grep -q "FrameClash" "$f"; then rm -f "$f"; fi
    systemctl --user unset-environment "${vars[@]}" 2>/dev/null || true
    echo "Proxy env OFF. Programs started before keep their old env until restarted (reboot to be sure)."
    ;;
  status)
    [[ -f "$f" ]] && echo "file: $f (present)" || echo "file: $f (absent)"
    systemctl --user show-environment 2>/dev/null | grep -i '_proxy=' || echo "user manager: no *_proxy vars"
    ;;
  *) echo "usage: $0 on|off|status" >&2; exit 2 ;;
esac
