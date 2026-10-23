# FrameClash guide

Details behind the [README](../README.md): the CLI, proxy and TUN modes, profile overrides, recovery,
full removal, and building / releasing.

## Why not a Decky plugin

Decky Loader has only experimental ARM builds and is not known to run on
the Frame; its UI hooks into gamepadui, while the Frame shows the SteamVR dashboard; and DeckyClash needs Decky's root
backend. FrameClash is standalone and runs as your user:

| | DeckyClash (Steam Deck) | FrameClash (Steam Frame) |
|---|---|---|
| Privileges | mihomo runs as root under Decky's root backend | user services; TUN uses a root-owned copy of mihomo with only `cap_net_admin`, set up once by you |
| Modes | always TUN | system proxy (`127.0.0.1:7890`) by default, TUN optional |
| TUN routing | `auto-route` + `auto-redirect` (nftables `inet mihomo`) | `auto-route` only: no nftables, firewalld's ruleset is never touched |
| Excluded from TUN | nothing | `wlanap` (PC streaming AP), `usb0`, private/link-local/multicast ranges |
| Force-kill | `pkill -KILL` leaves rules pointing at a dead tun: no network until reboot | the kernel removes the tun with mihomo, traffic falls back to `wlan0`; "Repair network" clears the rest without root |
| UI | Decky side panel | SteamVR dashboard panel, web page, CLI |

## CLI

`~/.local/bin/frameclash` is a symlink to `frameclashd`, which acts as the client when called by that name
(`~/.local/lib/frameclash/frameclashd cli ...` is the same). It only talks to the daemon on 127.0.0.1.

```
status                         is the proxy running, which profile, the proxy address
start | stop                   start / stop the proxy (stop also turns autostart off)
tun on|off                     TUN mode (needs tun-setup.sh install once)
mode rule|global|direct        outbound mode (saved, applied without a restart; default rule)
exit-ip                        where proxied traffic leaves to the internet (asks ipinfo.io)
tun dns on|off                 TUN: point systemd-resolved into the tun (default on), or leave it alone
repair                         stop the proxy and remove anything TUN left behind (no root)
import-url URL [--name NAME]   import a subscription (URL "-" = read it from stdin)
import-file PATH [--name NAME] import a Clash/mihomo YAML file under your home folder
profiles | list                saved profiles (* = active) and YAML files found
use ID|NAME                    switch profile (a subscription is downloaded again)
refresh                        download the active subscription again / re-read the active file
remove ID|NAME                 take a profile off the list (files are never deleted)
groups                         proxy groups and their selected node
nodes [GROUP]                  nodes of a group with last delay      (also: node list [GROUP])
select [GROUP] NODE            choose a node                         (also: node select [GROUP] NODE)
delay [GROUP]                  latency test, sorted
version
```

Exit code 0 = ok, 1 = failed, 2 = usage error. Over SSH use the full path (`~/.local/bin` is usually not in `PATH`
for `ssh host command`); reading the URL from stdin keeps it out of shell history and `ps`:

```sh
ssh steamos@<frame-ip> '~/.local/bin/frameclash status'
pbpaste | ssh steamos@<frame-ip> '~/.local/bin/frameclash import-url - --name Home'
```

## Web UI

frameclashd serves a web page on `http://127.0.0.1:7899/` (loopback only, Host-header and custom-header checks
against DNS rebinding/CSRF). Open it from your computer through SSH:

```sh
ssh -L 7899:127.0.0.1:7899 steamos@<frame-ip>     # then open http://127.0.0.1:7899/ locally
```

## System proxy mode (default)

mihomo listens on `127.0.0.1:7890` (HTTP + SOCKS5). Only programs that use a proxy are covered:

- per command: `curl -x http://127.0.0.1:7890 ...`, `https_proxy=http://127.0.0.1:7890 git clone ...`
- per user: `~/.local/lib/frameclash/proxy-env.sh on` writes `~/.config/environment.d/90-frameclash-proxy.conf`
  (reboot so the session inherits it; `off` reverts). While it is on, those programs lose network if FrameClash is stopped.
- browsers / KDE: their own proxy settings.

Games, UDP and programs that ignore proxy settings need TUN.

FrameClash always forces: `mixed-port`, `allow-lan: false`, `bind-address: 127.0.0.1`, controller
`127.0.0.1:9090` with a secret, `geo-auto-update: false`; it drops the profile's other listeners
(`port`, `socks-port`, `redir-port`, `tproxy-port`, `listeners`, `external-ui*`, `authentication`, `interface-name`,
`routing-mark`, ...). `dns.listen` is kept but moved to 127.0.0.1 (ports < 1024 dropped), because profiles point
`proxy-server-nameserver` at it. Two DIRECT rules for Steam's CDN detection are prepended (from DeckyClash). Ports
are set in `~/.config/frameclash/env`.

## TUN mode

The Frame has two Wi-Fi interfaces: `wlan0` (internet) and `wlanap` (its own 6 GHz AP for PC VR streaming). TUN
takes over `wlan0` only. Never routed into the tun: `wlanap`, `usb0`, 10/8, 172.16/12, 192.168/16, 100.64/10,
link-local, multicast, broadcast, fc00::/7, fe80::/10. `auto-redirect` is off, so no nftables rules are created and
firewalld's tables are untouched. Device `frameclash0`, route table 2022, ip rules from priority 9000 (fixed, so
cleanup removes exactly these).

mihomo needs `cap_net_admin` for this. A user service cannot grant it, so you set it up once yourself:

```sh
~/.local/lib/frameclash/tun-setup.sh install                 # asks for sudo; shows every command and mihomo's sha256 first
~/.local/lib/frameclash/tun-setup.sh install --no-self-dns   # the same without the polkit rule (no TUN DNS)
~/.local/bin/frameclash tun on                               # or the TUN switch in the panel / web page
~/.local/bin/frameclash tun off
~/.local/lib/frameclash/tun-setup.sh status                  # helper, polkit rule, resolved, leftover state (no sudo)
~/.local/lib/frameclash/tun-setup.sh cleanup                 # last resort: remove leftover rules/routes/device (sudo)
~/.local/lib/frameclash/tun-setup.sh remove                  # cleanup + delete /home/.frameclash and the polkit rule
```

`install` creates `/home/.frameclash/mihomo` (root-owned copy, `setcap cap_net_admin=ep`; `/home` survives A/B
updates and is not `nosuid`) and `/etc/polkit-1/rules.d/49-frameclash-resolved.rules`, which lets only your user
call resolved's `set-dns-servers` and `set-domains`. frameclashd refuses the copy unless the directory and file are
root-owned, not writable by others, not symlinks, and carry the capability. Re-run `install` after updating FrameClash.
Compare the sha256 it prints with the release before typing your password: `~/.local/lib` is writable by your user.

**TUN DNS** (`frameclash tun dns on|off`, the panel's DNS switch; default on): systemd-resolved sends its queries
bound to `wlan0`, which bypasses the tun, so apps would get the network's (possibly poisoned) answers. With TUN DNS
on, FrameClash sets DNS `198.18.0.2` and routing domain `~.` on the `frameclash0` link, so every lookup goes through
mihomo. `wlan0` is never changed; the settings vanish with the tun device. Without the polkit rule TUN still works and
web/QUIC traffic is still routed by domain through mihomo's sniffer.

Trade-offs: any process of your user can run the capable mihomo copy; if a future SteamOS mounted `/home` `nosuid`,
the kernel would ignore the capability and TUN would fail to come up (`getcap` would still show it on the file).

## override.yaml

`~/.config/frameclash/override.yaml` is merged over every profile before FrameClash's forced settings: maps merge
recursively, lists and scalars replace. The outbound `mode` is FrameClash's setting (panel or `frameclash mode`,
default rule): a `mode:` in the profile or in override.yaml is ignored.

**Do not use `system` as a nameserver on Linux** (in `nameserver`, `nameserver-policy`, `proxy-server-nameserver`,
...). It means systemd-resolved, which TUN DNS points back into mihomo: a loop, and DIRECT sites fail. It is meant
for Windows/macOS clients. If a subscription has it, override that entry:

```yaml
dns:
  nameserver-policy:
    "geosite:cn,apple,private": [119.29.29.29, 223.5.5.5]   # the profile's entry, minus "system"
```

## Crashes, suspend, reboot, power loss

After a crash, suspend, shutdown or power loss TUN never leaves the network broken (measured on the Frame): when
mihomo dies the kernel deletes the tun device and table 2022's routes, so the leftover ip rules point at an empty
table and traffic falls back to `wlan0`; a reboot clears all TUN state. FrameClash does no background monitoring and
makes no automatic decisions; recovery is up to you:

1. Open FrameClash and press **Start**: the proxy (and TUN, if it was on) comes back; mihomo replaces leftover rules.
2. If anything still looks wrong: **Repair network** in the VR panel, `frameclash repair`, or `POST /api/repair`.
   No root needed: it stops any mihomo and, if TUN rules 9000-9099 or the `frameclash0` device are left, briefly
   starts the setcap mihomo with a TUN-only config and SIGTERMs it, which removes them. FrameClash stays stopped.
3. Last resort: `~/.local/lib/frameclash/tun-setup.sh cleanup` (sudo), or reboot.

Stopping is always SIGTERM (mihomo then removes its TUN state itself), never SIGKILL. The unit uses
`KillMode=control-group`: with `mixed` + `SendSIGKILL=no`, a mihomo left over by a dead frameclashd made systemd
refuse to restart the daemon. frameclashd also SIGTERMs a leftover mihomo of its own run dir when it starts.

```sh
systemctl --user status frameclash     # journalctl --user -u frameclash -e; mihomo log: ~/.local/share/frameclash/mihomo.log
systemctl --user restart frameclash    # the proxy autostarts if it was running before
```

## Restore the Frame to exactly as before

```sh
~/.local/lib/frameclash/uninstall.sh --purge
rm -rf ~/frameclash-*-steamframe-aarch64 ~/frameclash-*-steamframe-aarch64.tar.gz ~/SHA256SUMS   # the package + panel build
```

`uninstall.sh` (no `--purge` keeps `~/.config/frameclash`) removes the proxy env file, runs `tun-setup.sh remove`
when started from a terminal (asks for sudo; otherwise it prints the commands), stops the panel and the daemon with
SIGTERM, removes units, launchers, the icon, `~/.local/lib/frameclash`, `~/.local/share/frameclash`, the
`frameclash` symlink (only if it is ours), and the XDG directories install.sh created if they are empty.

By hand, if the script is gone:

```sh
~/.local/bin/frameclash tun off; ~/.local/lib/frameclash/proxy-env.sh off
systemctl --user disable --now frameclash-panel.service frameclash.service
rm -rf ~/.local/lib/frameclash ~/.local/share/frameclash ~/.config/frameclash
rm -f ~/.local/bin/frameclash ~/.config/environment.d/90-frameclash-proxy.conf \
      ~/.config/systemd/user/frameclash.service ~/.config/systemd/user/frameclash-panel.service \
      ~/.config/systemd/user/steamvr.service.wants/frameclash-panel.service \
      ~/.local/share/applications/frameclash.desktop ~/.local/share/applications/frameclash-panel.desktop \
      ~/.local/share/icons/hicolor/256x256/apps/frameclash.png
systemctl --user daemon-reload
# TUN's root-owned parts (you run these):
sudo setcap -r /home/.frameclash/mihomo; sudo rm -rf /home/.frameclash
sudo rm -f /etc/polkit-1/rules.d/49-frameclash-resolved.rules
# TUN state in the kernel, if any (or just reboot):
sudo ip rule del priority <9000-9099>; sudo ip route flush table 2022; sudo ip link del frameclash0
```

Verify (each should show nothing / "No such file" / non-zero exit):

```sh
ip rule; ip -6 rule                              # no priorities 9000-9099
ip route show table 2022; ip -6 route show table 2022
ip -br link | grep frameclash0
ss -ltn | grep -E ':(7890|7899|9090)\b'
pgrep -af 'mihomo|frameclash'
systemctl --user list-unit-files | grep frameclash
systemctl --user show-environment | grep -i _proxy
ls -d ~/.local/lib/frameclash ~/.local/share/frameclash ~/.config/frameclash ~/.local/bin/frameclash \
      ~/.config/systemd/user/frameclash.service ~/.config/systemd/user/frameclash-panel.service \
      ~/.config/systemd/user/steamvr.service.wants/frameclash-panel.service \
      ~/.local/share/applications/frameclash.desktop ~/.local/share/applications/frameclash-panel.desktop \
      ~/.local/share/icons/hicolor/256x256/apps/frameclash.png ~/.config/environment.d/90-frameclash-proxy.conf
ls -ld /home/.frameclash; getcap -r /home/.frameclash
pkcheck --action-id org.freedesktop.resolve1.set-domains --process $$; echo "rule: $?"   # non-zero = no rule
resolvectl status                                # no frameclash0 link; wlan0's DNS as the network set it
```

`/etc/polkit-1/rules.d` is readable by root only, so `pkcheck` is how you see the rule without sudo. If install.sh
created `~/.local/share/applications/mimeinfo.cache`, uninstall removes it once no `.desktop` files are left there.

## Build from source

```sh
tools/make-package.sh   # on macOS or Linux; needs Go and curl
# -> dist/frameclash-<version>-steamframe-aarch64.tar.gz + dist/SHA256SUMS
```

It cross-builds `frameclashd` (static, linux/arm64), downloads the official `mihomo-linux-arm64-v1.19.30.gz` and the
MetaCubeX geo data once into `vendor/`, and packs the panel **as source**: `install.sh` compiles it on the Frame
(like frameeyeosc), linking SteamVR's own `libopenvr_api.so`.

**Releasing:** set the version in `var version` (`src/frameclashd/main.go`) and `project(... VERSION ...)`
(`src/frameclash-panel/CMakeLists.txt`), add a `## X.Y.Z` section to `CHANGELOG.md`, commit, then
`git tag vX.Y.Z && git push origin main vX.Y.Z`. GitHub Actions tests, packages and publishes the release with that
section as its notes; it refuses a tag that doesn't match the version or has no changelog section.

## Testing

- **Go:** the daemon uses Linux-only syscalls, so run its tests in Linux, e.g. with Docker from `src/frameclashd`:
  `docker run --rm -v "$PWD":/src -w /src golang:1.27 go test ./...`
- **Panel:** `install.sh` builds and runs `frameclash-panel-tests` on the Frame and stops if they fail. Elsewhere:
  `cmake -G Ninja -S src/frameclash-panel -B build/panel -DOPENVR_LIB_DIR=<dir with libopenvr_api.so> && ninja -C build/panel && build/panel/frameclash-panel-tests`.
  `frameclash-panel --dump-png out.png [--do STEP]...` renders the panel against a live daemon without SteamVR.
  On macOS/Linux with Docker, `tools/panel-preview/run.sh out.png --lang zh --do view:proxies` builds the panel
  against a stub OpenVR, runs frameclashd + mihomo with fake nodes and writes `build/preview/out.png`.
- **Scripts:** `FRAMECLASH_NO_SYSTEMCTL=1 FRAMECLASH_ALLOW_ANY_ARCH=1 HOME=/tmp/x ./install.sh` runs the installer
  against a scratch home.

## Repository layout

```
src/frameclashd/        Go daemon + CLI + web UI (ui/index.html embedded); TUN in tun.go, recovery in recovery.go
src/frameclash-panel/   C++17 SteamVR dashboard panel (cairo, FreeType, Vulkan, OpenVR); tests/, contrib/ (unit, launcher)
pkg/                    install.sh, uninstall.sh, tun-setup.sh, proxy-env.sh, unit, launcher, env.example, icon
tools/make-package.sh   builds dist/frameclash-<version>-steamframe-aarch64.tar.gz + SHA256SUMS
.github/workflows/      CI (Go tests, panel build + tests on arm64, package); a pushed tag vX.Y.Z publishes a release
```

## Credits

- [DeckyClash](https://github.com/chenx-dust/DeckyClash): the original idea and reference for the Steam Deck.
- [frameeyeosc](https://github.com/sasaken1102r/frameeyeosc) (MIT): the panel's drawing, Vulkan texture and OpenVR
  overlay code, and the on-device build approach.
- [mihomo](https://github.com/MetaCubeX/mihomo) and [meta-rules-dat](https://github.com/MetaCubeX/meta-rules-dat) by MetaCubeX.
- [clash-verge-rev](https://github.com/clash-verge-rev/clash-verge-rev): reference for profile overrides.
