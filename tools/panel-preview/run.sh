#!/usr/bin/env bash
# Render the VR panel to a PNG on macOS/Linux with Docker (arm64 host or emulation), no SteamVR needed:
#   tools/panel-preview/run.sh overview.png --lang zh
#   tools/panel-preview/run.sh proxies.png --do view:proxies --do test
# Builds frameclashd (Go) and the panel against a stub libopenvr_api, starts mihomo with test.yaml (fake nodes),
# and writes build/preview/OUT.png. Needs vendor/mihomo-linux-arm64-*.gz (tools/make-package.sh fetches it).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tools="$root/tools/panel-preview" out="$root/build/preview"
mkdir -p "$out"
gz=$(ls "$root"/vendor/mihomo-linux-arm64-*.gz 2>/dev/null | tail -1) || true
[[ -n "$gz" ]] || { echo "no vendor/mihomo-linux-arm64-*.gz: run tools/make-package.sh once" >&2; exit 1; }
[[ -x "$out/mihomo" ]] || gunzip -c "$gz" > "$out/mihomo" && chmod 755 "$out/mihomo"
(cd "$root/src/frameclashd" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -trimpath -o "$out/frameclashd" .)
docker build -q --platform linux/arm64 -t frameclash-panel-dev "$tools" >/dev/null
docker run --rm --platform linux/arm64 -v "$root/src/frameclash-panel":/src:ro -v "$tools":/tools:ro -v "$out":/build \
  frameclash-panel-dev sh -c '
    set -e
    mkdir -p /build/ovr
    g++ -shared -fPIC -I/src/third_party/openvr -o /build/ovr/libopenvr_api.so /tools/openvr_stub.cpp
    cmake -G Ninja -S /src -B /build/panel -DOPENVR_LIB_DIR=/build/ovr >/dev/null
    ninja -C /build/panel >/dev/null
    sh /tools/preview.sh "$@"' sh "$@"
echo "$out/$1"
