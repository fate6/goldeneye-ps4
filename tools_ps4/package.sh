#!/usr/bin/env bash
# Package build-ps4's eboot as GOLD00064 ("GoldenEye 007") and optionally
# upload it to the console's /data/pkg. No game data goes in the package:
# the user's ROM and the converted sidecars live in /data/GoldenEye64/data
# (tools_ps4/upload-data.sh).
#   tools_ps4/package.sh <version XX.YY> [upload]
# GE_PS4_ICON: 512x512 icon0.png (not in the repo; default: the local one).
set -euo pipefail
source "$HOME/DolphinPS4/scripts/ps4-env.sh"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${GE_PS4_BUILD:-$SRC/build-ps4}"
VERSION="${1:?version XX.YY}"
ICON="${GE_PS4_ICON:-/mnt/c/claude/PS4_ports/n64/goldeneye/ps4/sce_sys/icon0.png}"
HOST="ftp://${PS4_HOST:-10.0.0.150}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/out"

eboot="$BUILD/ge007_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "no eboot.bin: run tools_ps4/build.sh first" >&2; exit 1; }
[ -f "$ICON" ] || { echo "no icon at $ICON (set GE_PS4_ICON)" >&2; exit 1; }

stage="$BUILD/stage"
rm -rf "$stage"
mkdir -p "$stage/sce_sys"
cp "$eboot" "$stage/"
cp "$ICON" "$stage/sce_sys/icon0.png"
# Plain homebrew identity: Vulkan through RADV, no Piglet modules bundled.
pkg="$(SFO_STYLE=plain DOLPHIN_PS4_MODULES_DIR=/nonexistent \
    "$HOME/DolphinPS4/scripts/make-pkg.sh" "$stage" GOLD00064 "GoldenEye 007" "$VERSION" \
    GOLDENEYE64 "$OUT" | tail -1)"
echo "package: $pkg"
if [ "${2:-}" = upload ]; then
    curl -sS --max-time 300 -T "$pkg" "$HOST/data/pkg/"
    echo "uploaded $(basename "$pkg")"
fi
