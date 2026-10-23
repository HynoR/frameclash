#!/usr/bin/env bash
# Build dist/frameclash-<version>-steamframe-aarch64.tar.gz + dist/SHA256SUMS on any machine with Go and curl (macOS or Linux).
# frameclashd is cross-built here; mihomo and the geo data are downloaded once into vendor/; the VR panel ships as
# source and install.sh builds it on the Frame (SteamOS has cmake, ninja, g++ and the -dev files), like frameeyeosc.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
ver="$(sed -n 's/^var version = "\(.*\)"/\1/p' "$root/src/frameclashd/main.go")"
mihomo_ver=v1.19.30
mihomo_sha256=58896873736d28628f66de3677c8654fa0f180662523148e136cff4f6e890069  # of the .gz, checked before use
name="frameclash-$ver-steamframe-aarch64"
out="$root/build/$name"
vendor="$root/vendor"
sha() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

mkdir -p "$vendor" "$root/dist"
fetch() { [[ -s "$vendor/$1" ]] || { echo "==> downloading $1"; curl -fsSL -o "$vendor/$1.part" "$2" && mv "$vendor/$1.part" "$vendor/$1"; }; }
fetch "mihomo-linux-arm64-$mihomo_ver.gz" \
  "https://github.com/MetaCubeX/mihomo/releases/download/$mihomo_ver/mihomo-linux-arm64-$mihomo_ver.gz"
[[ "$(sha "$vendor/mihomo-linux-arm64-$mihomo_ver.gz" | cut -d' ' -f1)" == "$mihomo_sha256" ]] ||
  { echo "mihomo-linux-arm64-$mihomo_ver.gz: sha256 mismatch" >&2; exit 1; }
fetch Country.mmdb https://github.com/MetaCubeX/meta-rules-dat/releases/download/latest/country.mmdb
fetch GeoSite.dat https://github.com/MetaCubeX/meta-rules-dat/releases/download/latest/geosite.dat
fetch "LICENSE.mihomo-$mihomo_ver" "https://raw.githubusercontent.com/MetaCubeX/mihomo/$mihomo_ver/LICENSE"  # GPL-3.0 text

rm -rf "$out"; mkdir -p "$out/bin" "$out/geo" "$out/icons" "$out/docs"
echo "$ver" > "$out/VERSION"
(cd "$root/src/frameclashd" &&
  GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' -o "$out/bin/frameclashd" .)
gunzip -c "$vendor/mihomo-linux-arm64-$mihomo_ver.gz" > "$out/bin/mihomo" && chmod 755 "$out/bin/mihomo"
cp "$vendor/Country.mmdb" "$vendor/GeoSite.dat" "$out/geo/"
cp "$root/pkg/icons/frameclash.png" "$out/icons/"
for f in install.sh uninstall.sh proxy-env.sh tun-setup.sh; do install -m755 "$root/pkg/$f" "$out/$f"; done
for f in env.example frameclash.service; do install -m644 "$root/pkg/$f" "$out/$f"; done
for f in README.md LICENSE THIRD_PARTY_NOTICES.md docs/GUIDE.md; do install -m644 "$root/$f" "$out/$f"; done
install -m644 "$vendor/LICENSE.mihomo-$mihomo_ver" "$out/LICENSE.mihomo"
panel="$root/src/frameclash-panel"
cp "$panel/contrib/frameclash-panel.service" "$panel/contrib/frameclash-panel.desktop" "$out/"
mkdir -p "$out/panel-src"
(cd "$panel" && tar --exclude build --exclude .DS_Store -cf - .) | tar -C "$out/panel-src" -xf -
(cd "$out" && find . -type f ! -name SHA256SUMS | sort | while read -r f; do sha "$f"; done > SHA256SUMS)
COPYFILE_DISABLE=1 tar -C "$root/build" --exclude .DS_Store -czf "$root/dist/$name.tar.gz" "$name"
(cd "$root/dist" && sha "$name.tar.gz" | tee SHA256SUMS && ls -l "$name.tar.gz")
