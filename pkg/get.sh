#!/usr/bin/env bash
# One-line install or update of FrameClash on the Steam Frame (Konsole or SSH, as user steamos; never sudo):
#   curl -fsSL https://github.com/HynoR/frameclash/releases/latest/download/get.sh | bash
#   ... | bash -s -- --no-panel          options go to install.sh
#   ... | FRAMECLASH_VERSION=0.6.0 bash  a given release instead of the latest
# Downloads the release tarball + SHA256SUMS, checks the sum, and runs install.sh from a temporary folder.
set -euo pipefail

# everything sits in main, so a download cut off half way runs nothing
main() {
  local repo=https://github.com/HynoR/frameclash ver="${FRAMECLASH_VERSION:-}" tmp name
  if [[ -z "$ver" ]]; then
    # /releases/latest redirects to /releases/tag/vX.Y.Z (no API token, no jq needed)
    ver=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$repo/releases/latest")
    ver=${ver##*/v}
  fi
  [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "could not find the latest FrameClash release" >&2; exit 1; }
  name="frameclash-$ver-steamframe-aarch64"
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  echo "==> Downloading FrameClash $ver"
  curl -fL --progress-bar -o "$tmp/$name.tar.gz" "$repo/releases/download/v$ver/$name.tar.gz"
  curl -fsSL -o "$tmp/SHA256SUMS" "$repo/releases/download/v$ver/SHA256SUMS"
  (cd "$tmp" && sha256sum -c --ignore-missing SHA256SUMS) || { echo "checksum mismatch: not installing" >&2; exit 1; }
  tar xzf "$tmp/$name.tar.gz" -C "$tmp"
  "$tmp/$name/install.sh" "$@"
}

main "$@"
