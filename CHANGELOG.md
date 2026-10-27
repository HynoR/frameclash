# Changelog

A release is made by pushing a tag `vX.Y.Z` whose `## X.Y.Z` section below becomes the release notes
(`.github/workflows/build.yml`).

## 0.6.0

- New **TUN Setup** panel: helper version, polkit DNS permission and leftover checks, suggested install / repair /
  cleanup actions, and copyable full-path commands for the Frame's own terminal. Adds `GET /api/tun/setup` and
  `frameclash tun setup`. The panel copies a command to the clipboard (the X CLIPBOARD, which gamescope shares), ready to paste into Konsole.
- New **Connections** panel and CLI: group by process, host, rule or chain, inspect newest-first details, and close a
  single connection, a group, or everything. The daemon parses mihomo's full response and returns only bounded results.
- New **Logs** panel and CLI: follow or scroll the current log, read the previous log, filter levels, clear, or change
  mihomo's level temporarily. Log reads are tail-only and bounded; starts append markers instead of truncating, with
  one `.prev` file rotated at the next start after 4 MiB. Clearing is safe while mihomo keeps writing (no NUL gap).
- mihomo now runs at `log-level: silent` by default (logs only when asked); `override.yaml` or the Logs panel's level
  button turn it back on.
- One-line install / update: `curl -fsSL https://github.com/HynoR/frameclash/releases/latest/download/get.sh | bash`
  (`pkg/get.sh`, published with each release; checks SHA256SUMS, then runs `install.sh`).
- The sidebar's language button is now a 中文 / EN switch.
- Preview fixtures now keep real local connections open, and Go / panel tests cover the new parsers, grouping,
  closing, TUN suggestions and log rotation / truncation.

## 0.5.0

First public release: mihomo (Clash.Meta) on the Steam Frame, controlled from the SteamVR dashboard, a local web page
or the `frameclash` command. Nothing needs sudo except the optional, one-time TUN setup that you run yourself.

- VR panel: a sidebar with Overview / Proxies / Profiles. Overview has start/stop, upload/download speed, the TUN and
  TUN DNS switches, outbound mode (Rule / Global / Direct, applied without a restart), the current node, an
  on-demand exit-IP check and "Repair network" (two taps). Proxies has the groups, a node grid, latency tests and
  sort by latency. Profiles imports a subscription URL or YAML by Paste. When frameclashd stops answering, the
  panel offers "Restart service" (no root). English and Chinese.
- TUN mode (optional): everything on `wlan0` goes through the proxy, including UDP; the streaming AP `wlanap`,
  `usb0` and private ranges stay direct, and firewalld's nftables are untouched. systemd-resolved is pointed into
  the tun so DNS isn't bypassed.
- Recovery is in your hands, with no background watchdog: if mihomo dies the kernel removes the tun and traffic
  falls back to `wlan0`; "Repair network" (or `frameclash repair`) clears leftover rules without root and leaves
  the proxy off; a reboot clears everything.
- `~/.config/frameclash/override.yaml` is merged over every profile. The outbound mode is FrameClash's setting: a
  profile's `mode:` is ignored.
- `uninstall.sh --purge` plus `tun-setup.sh remove` restore the headset to exactly as before (docs/GUIDE.md: "Restore
  the Frame to exactly as before").
