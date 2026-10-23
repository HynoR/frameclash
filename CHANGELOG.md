# Changelog

A release is made by pushing a tag `vX.Y.Z` whose `## X.Y.Z` section below becomes the release notes
(`.github/workflows/build.yml`).

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
