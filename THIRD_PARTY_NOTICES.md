# Third-party notices

FrameClash's own code (`frameclashd`, `frameclash-panel`, the scripts in `pkg/` and `tools/`) is MIT-licensed, see
`LICENSE`. The release tarball (`frameclash-<version>-steamframe-aarch64.tar.gz`) and the source tree contain or use the
following third-party work.

## mihomo (GPL-3.0): shipped as an unmodified binary

- `bin/mihomo` in the release tarball is the official, **unmodified** release binary
  `mihomo-linux-arm64-v1.19.30.gz` from https://github.com/MetaCubeX/mihomo/releases/tag/v1.19.30 (decompressed only).
  `tools/make-package.sh` downloads it from that URL. FrameClash runs it as a separate program; it is not linked into
  any FrameClash code.
- License: GNU General Public License v3.0 (the `LICENSE` file at tag `v1.19.30`, shipped in the tarball as
  `LICENSE.mihomo`).
- **Corresponding source:** the exact source of this binary is the `v1.19.30` tag of
  https://github.com/MetaCubeX/mihomo (https://github.com/MetaCubeX/mihomo/tree/v1.19.30, also downloadable as
  https://github.com/MetaCubeX/mihomo/archive/refs/tags/v1.19.30.tar.gz). If that location ever stops being
  available, ask the FrameClash maintainers for a copy of the corresponding source.
- With TUN set up, `tun-setup.sh install` copies the same binary, unchanged, to `/home/.frameclash/mihomo` and adds a
  file capability (`cap_net_admin`); the file content is not modified.

## MetaCubeX geo data (`geo/Country.mmdb`, `geo/GeoSite.dat`): shipped data files

- Downloaded by `tools/make-package.sh` from the `latest` release of https://github.com/MetaCubeX/meta-rules-dat
  (`country.mmdb`, `geosite.dat`). The download is not pinned to a dated release; the tarball's `SHA256SUMS`
  identifies the exact files shipped. They are installed unmodified into `~/.local/share/frameclash/mihomo/`.
- The meta-rules-dat repository's `LICENSE` is the **GNU GPL v3.0** (checked on GitHub when 0.5.0 was prepared).
- Upstream data, as credited by meta-rules-dat: `Country.mmdb` is the same as Loyalsoldier/v2ray-rules-dat's, built
  with Loyalsoldier/geoip (repository licensed CC-BY-SA-4.0 and GPL-3.0) from **MaxMind GeoLite2** country data;
  `GeoSite.dat` is built mainly from v2fly/domain-list-community (MIT) plus the other lists credited in the
  meta-rules-dat README. **Not verified:** whether MaxMind's GeoLite2 terms add attribution or redistribution
  conditions for these derived files. Check https://www.maxmind.com/en/geolite2/eula before redistributing; to
  avoid shipping them, delete `geo/` from the package and let mihomo download them on first start.

## gopkg.in/yaml.v3 v3.0.1 (MIT and Apache-2.0): compiled into `frameclashd`

https://github.com/go-yaml/yaml (branch v3). Its `LICENSE` and `NOTICE` files:

```

This project is covered by two different licenses: MIT and Apache.

#### MIT License ####

The following files were ported to Go from C files of libyaml, and thus
are still covered by their original MIT license, with the additional
copyright staring in 2011 when the project was ported over:

    apic.go emitterc.go parserc.go readerc.go scannerc.go
    writerc.go yamlh.go yamlprivateh.go

Copyright (c) 2006-2010 Kirill Simonov
Copyright (c) 2006-2011 Kirill Simonov

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### Apache License ###

All the remaining project files are covered by the Apache license:

Copyright (c) 2011-2019 Canonical Ltd

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

```
Copyright 2011-2016 Canonical Ltd.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

## frameeyeosc (MIT): code copied and adapted into `frameclash-panel`

Paths below are relative to `src/frameclash-panel/`.

- Project: https://github.com/sasaken1102r/frameeyeosc (fork of konsti219's original), commit `c4294a9`.
- Copied **unmodified** into `src/third_party_frameeyeosc/` (each file starts with an attribution line):
  `draw.{h,cpp}`, `icons.{h,cpp}`, `text_layout.{h,cpp}`, `json.{h,cpp}`, `vk_texture.{h,cpp}`, `theme.h`.
  The license text is kept next to them as `src/third_party_frameeyeosc/LICENSE.frameeyeosc`.
- **Adapted** (rewritten for FrameClash, keeping the structure; the header of each file says so):
  `src/vr_overlay.{h,cpp}` (from `panel/src/vr_overlay.cpp`: OpenVR init order, dashboard overlay, laser-as-mouse
  input, shutdown order), `src/spawn.{h,cpp}` (from `panel/src/setup_tools.cpp`: launching a program detached via
  `systemd-run --user`), `contrib/frameclash-panel.service` (from `panel/contrib/frameeyeosc-panel.service`), and the
  overall look in `src/ui.cpp` / `src/main.cpp`.

```
MIT License

Copyright (c) 2026 konsti219
Copyright (c) 2026 sasaken1102r (changes in this fork)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## OpenVR SDK header (BSD-3-Clause)

- `src/frameclash-panel/third_party/openvr/openvr.h` (OpenVR SDK 2.15.6) from https://github.com/ValveSoftware/openvr;
  the license is kept next to it as `third_party/openvr/LICENSE`. Only the header is included. At build and run time
  the panel uses SteamVR's own `libopenvr_api.so` from `/opt/steamvr/bin/linuxarm64`; FrameClash does not ship it.

```
Copyright (c) 2015, Valve Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Libraries used from the system (not shipped)

The panel is compiled on the Frame by `install.sh` and links the system's cairo (LGPL-2.1 / MPL-1.1), FreeType
(FTL / GPL-2.0), the Vulkan loader (Apache-2.0), libxcb (MIT) and glibc (LGPL-2.1); libstdc++/libgcc are linked
statically from the Frame's GCC (GPL-3.0 with the GCC Runtime Library Exception). Fonts: Noto Sans CJK from the
system (SIL OFL 1.1). None of these are part of the release tarball.

## Consulted for reference only: no code copied

- **DeckyClash** (https://github.com/chenx-dust/DeckyClash, the Steam Deck Decky plugin FrameClash's idea comes
  from): read for its design; FrameClash reuses a few configuration values from it (default DNS servers, fake-ip
  filter patterns, the two "skip Steam CDN detection" DIRECT rules), no code.
- **clash-verge-rev** (https://github.com/clash-verge-rev/clash-verge-rev, GPL-3.0): reference for the profile
  override idea (`override.yaml`), no code.
- **WayVR** (https://github.com/wayvr-org/wayvr, GPL-3.0): read only as background on OpenVR overlays on Linux and
  the Frame; no WayVR code, assets or text are included.
- Valve's frame-developer-tools `gamescope_helpers` (`gamescope --backend openvr`): not used (needs root or the
  `steamvr` user).
