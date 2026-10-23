# FrameClash

FrameClash runs [mihomo](https://github.com/MetaCubeX/mihomo) (Clash.Meta) on the **Steam Frame** VR headset and
lets you control it from a panel in the SteamVR dashboard. It is inspired by
[DeckyClash](https://github.com/chenx-dust/DeckyClash) for the Steam Deck, but runs standalone as your user: no
Decky, no root service.

## Features

- **VR panel** in the SteamVR dashboard:
  - **Overview**: start/stop, live speed, TUN and DNS switches, outbound mode (Rule / Global / Direct), the current
    node, an exit-IP check and "Repair network".
  - **Proxies**: proxy groups, node selection, latency tests.
  - **Profiles**: import a subscription URL or a Clash/mihomo YAML file, and switch, refresh or remove saved profiles.
- **System proxy** on `127.0.0.1:7890` (HTTP + SOCKS5) by default.
- **TUN mode** (optional) for games and UDP. Only `wlan0` goes through the proxy; the PC-streaming AP (`wlanap`),
  `usb0` and the LAN stay direct.
- **`frameclash` command** for SSH, and a loopback web page.
- **No sudo** to install, use, update or uninstall. Everything lives under `~`; only the optional TUN setup asks for
  your password, once.
- **Recovery is up to you**: there is no background watchdog. If mihomo dies, traffic falls back to `wlan0`, and
  "Repair network" cleans up without root.

## Install

Requires a Steam Frame (SteamOS VR, aarch64) with SteamVR. Each
[release](https://github.com/HynoR/frameclash/releases) has `frameclash-<version>-steamframe-aarch64.tar.gz` and
`SHA256SUMS`. Use the release's version in place of `0.5.0`.

On the Frame, open Konsole or SSH in. To see Konsole in the app list, first turn on Steam Settings → System →
Enable Developer Mode. Then:

```sh
ver=0.5.0
base=https://github.com/HynoR/frameclash/releases/download/v$ver
curl -fLO $base/frameclash-$ver-steamframe-aarch64.tar.gz && curl -fLO $base/SHA256SUMS
sha256sum -c SHA256SUMS
tar xzf frameclash-$ver-steamframe-aarch64.tar.gz && cd frameclash-$ver-steamframe-aarch64
./install.sh
```

`sha256sum -c SHA256SUMS` must print `OK`. `install.sh` needs no sudo: it builds the VR panel (about a minute), then
installs and starts the service.

If the Frame can't reach GitHub, download both files on a computer and copy them over:
`scp frameclash-*-steamframe-aarch64.tar.gz SHA256SUMS steamos@<frame-ip>:~`. Then run the commands above from
`sha256sum -c` on.

To update, repeat the same steps with a newer release; your profiles and settings are kept. To uninstall, run
`~/.local/lib/frameclash/uninstall.sh --purge`.

## Usage

1. Open the SteamVR dashboard and pick **FrameClash**. You can also start it from the app list.
2. **Profiles**: copy your subscription URL, press **Paste** next to the link field, then **Import subscription**.
   To use a YAML file under `~` instead, switch to the config-file tab and paste its path.
3. **Overview**: press **Start**. Apps that support a proxy can then use `127.0.0.1:7890`.
4. **Proxies**: pick a node, or press **Test all** to measure latency.
5. Optional: to send all traffic (games, UDP) through the proxy, set up TUN once:

   ```sh
   ~/.local/lib/frameclash/tun-setup.sh install
   ```

   This step asks for sudo. Afterwards, turn on the TUN switch in the panel.

If something goes wrong, press **Repair network**. It needs a second tap to confirm, then stops the proxy and
removes leftover TUN state. Press Start to bring the proxy back.

Over SSH, run `~/.local/bin/frameclash status`, `start`, `stop`, `mode rule|global|direct`, `import-url -` and so on.

The CLI reference, TUN details, `override.yaml`, how to restore the headset exactly, and building from source are in
[docs/GUIDE.md](docs/GUIDE.md).

## License

MIT, see [LICENSE](LICENSE). The release tarball ships mihomo (GPL-3.0, unmodified) and the MetaCubeX geo data; the
panel uses code from [frameeyeosc](https://github.com/sasaken1102r/frameeyeosc) (MIT). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
