#!/usr/bin/env bash
# FrameClash TUN helper. Run it YOURSELF in a terminal: it shows what it will do and calls sudo only for that.
#   tun-setup.sh install [--no-self-dns]
#                          one-time: root-owned copy of mihomo in /home/.frameclash with cap_net_admin, and a polkit
#                          rule letting you (no password) point systemd-resolved at the tun link while TUN is on.
#                          --no-self-dns: no rule (removes an existing one); the system DNS stays as the network
#                          set it, and web/QUIC traffic is still routed by the sniffer (for networks whose DNS you trust)
#   tun-setup.sh cleanup   turn TUN off, then remove anything TUN left behind (after a crash / kill / power loss):
#                          ip rules 9000-9099, routes in table 2022, the frameclash0 device. Only these, only if present
#   tun-setup.sh remove    cleanup + delete /home/.frameclash: back to no TUN at all
#   tun-setup.sh status    helper + network state (no sudo)
# Same pattern as eyecam's /home/.eyecam on the Frame. Re-run "install" after a FrameClash update.
# Names/numbers must match src/frameclashd/tun.go (tunDevice, tunTable, tunRuleIndex).
set -euo pipefail
src="$HOME/.local/lib/frameclash/mihomo"
dir="$(dirname "$HOME")/.frameclash"
bin="$dir/mihomo"
dev=frameclash0
table=2022
cli="$HOME/.local/bin/frameclash"
rule=/etc/polkit-1/rules.d/49-frameclash-resolved.rules
user="$(id -un)"
# resolved sends DNS bound to wlan0, past the tun; frameclashd sets DNS + "~." on frameclash0 instead (runtime only,
# gone with the device). These are the only actions allowed, only for this user.
rule_js="// FrameClash TUN (tun-setup.sh): let $user set DNS on links via systemd-resolved. Remove: tun-setup.sh remove
polkit.addRule(function(action, subject) {
    if (subject.user == \"$user\" &&
        (action.id == \"org.freedesktop.resolve1.set-dns-servers\" ||
         action.id == \"org.freedesktop.resolve1.set-domains\")) {
        return polkit.Result.YES;
    }
});"
[[ "$(id -u)" != 0 ]] || { echo "Run as your normal user (steamos); the script calls sudo itself." >&2; exit 1; }

# TUN state still in the kernel: rule priorities 9000-9099 (none on a stock Frame), table 2022, the device
leftovers() {
  for fam in -4 -6; do
    ip "$fam" rule show | awk -v f="$fam" '$1 ~ /^90[0-9][0-9]:$/ { sub(":", "", $1); print "rule", f, $1 }'
    ip "$fam" route show table "$table" 2>/dev/null | grep -q . && echo "table $fam $table"
  done
  ip link show "$dev" >/dev/null 2>&1 && echo "link - $dev"
  return 0
}

cleanup() {
  if [[ -x "$cli" ]]; then "$cli" tun off >/dev/null 2>&1 || true; sleep 2; fi  # SIGTERM path: mihomo cleans up itself
  local left; left="$(leftovers)"
  if [[ -z "$left" ]]; then echo "Network is clean (no TUN rules, routes or device left)."; return 0; fi
  if pgrep -u "$(id -u)" -f "^$bin" >/dev/null; then
    echo "A TUN mihomo is still running ($(pgrep -u "$(id -u)" -f "^$bin" | tr '\n' ' ')): it owns this state." >&2
    echo "Stop it first:  systemctl --user stop frameclash   (or: pkill -TERM -f $bin)" >&2
    return 1
  fi
  echo "Left behind by TUN (will be removed with sudo):"
  printf '  %s\n' "$left"
  while read -r kind fam what; do
    case "$kind" in
      rule) sudo ip "$fam" rule del priority "$what" ;;
      table) sudo ip "$fam" route flush table "$what" ;;
      link) sudo ip link del "$what" ;;
    esac
  done <<< "$left"
  if command -v nft >/dev/null && sudo nft list table inet mihomo >/dev/null 2>&1; then
    sudo nft delete table inet mihomo && echo "  removed nft table inet mihomo"
  fi
  left="$(leftovers)"
  [[ -z "$left" ]] && echo "Network restored." || { echo "Still present:"; printf '  %s\n' "$left"; return 1; }
}

# the rules dir is root-only (0700): ask polkit whether the rule is in effect instead of looking for the file
has_rule() { pkcheck --action-id org.freedesktop.resolve1.set-domains --process $$ >/dev/null 2>&1; }
self_dns=true
[[ "${2:-}" != --no-self-dns ]] || self_dns=false
case "${1:-status}" in
  install)
    [[ -x "$src" ]] || { echo "$src not found: install FrameClash first" >&2; exit 1; }
    echo "Will run with sudo:"
    echo "  install -d -o root -g root -m 0755 $dir"
    echo "  install -o root -g root -m 0755 $src $bin"
    echo "  setcap cap_net_admin=ep $bin"
    if $self_dns; then
      echo "  write $rule:"
      printf '%s\n' "$rule_js" | sed 's/^/      /'
    else
      echo "  rm -f $rule   (--no-self-dns)"
    fi
    echo "mihomo sha256: $(sha256sum "$src" | cut -d' ' -f1)"
    read -rp "Continue? [y/N] " yes
    [[ "$yes" == [yY] ]] || exit 1
    sudo install -d -o root -g root -m 0755 "$dir"
    if [[ -e "$bin" ]]; then sudo setcap -r "$bin" 2>/dev/null || true; fi
    sudo install -o root -g root -m 0755 "$src" "$bin"
    sudo setcap cap_net_admin=ep "$bin"
    if $self_dns; then
      printf '%s\n' "$rule_js" | sudo tee "$rule" >/dev/null
      sudo chmod 644 "$rule"
    else
      sudo rm -f "$rule"
    fi
    getcap "$bin"
    echo "Done. Turn it on with:  $cli tun on   (off: $cli tun off; undo everything: $0 remove)"
    ;;
  cleanup)
    cleanup
    ;;
  remove)
    cleanup
    [[ -e "$dir" ]] || has_rule || { echo "Nothing else to remove ($dir does not exist)."; exit 0; }
    echo "Will run with sudo:  setcap -r $bin; rm -rf $dir; rm -f $rule"
    if [[ -e "$bin" ]]; then sudo setcap -r "$bin" 2>/dev/null || true; fi
    sudo rm -rf "$dir"
    sudo rm -f "$rule"
    echo "Removed $dir. TUN is fully uninstalled."
    ;;
  status)
    if [[ -e "$bin" ]]; then ls -ld "$dir" "$bin"; getcap "$bin"; else echo "helper: not set up ($bin missing)"; fi
    if has_rule; then echo "self DNS: allowed ($rule)"; else echo "self DNS: not allowed (--no-self-dns: system DNS left as is)"; fi
    resolvectl status "$dev" 2>/dev/null | grep -E "DNS Servers|DNS Domain" | sed 's/^ */resolved on '"$dev"': /' || true
    left="$(leftovers)"
    if [[ -z "$left" ]]; then echo "network: no TUN state"; else echo "network: TUN state present"; printf '  %s\n' "$left"; fi
    ;;
  *) echo "usage: $0 install [--no-self-dns] | cleanup | remove | status" >&2; exit 2 ;;
esac
