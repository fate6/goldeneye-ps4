#!/usr/bin/env bash
# Package build-ps4's eboot as GOLD00064 ("GoldenEye 007") and optionally
# upload it to the console's /data/pkg. No game data goes in the package:
# the user's ROM and the converted sidecars live in /data/GoldenEye64/data
# (tools_ps4/upload-data.sh).
#   tools_ps4/package.sh <version XX.YY> [upload]
# Package art: tools_ps4/sce_sys/ holds original placeholder art
# (make-placeholder-art.py). A private build can use its own: every PNG in
# tools_ps4/sce_sys.local/ (gitignored, needs icon0.png) replaces the whole set,
# or GE_PS4_ART=<dir> names another folder.
set -euo pipefail
source "$HOME/DolphinPS4/scripts/ps4-env.sh"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${GE_PS4_BUILD:-$SRC/build-ps4}"
VERSION="${1:?version XX.YY}"
ART="$SRC/tools_ps4/sce_sys"
[ -d "$SRC/tools_ps4/sce_sys.local" ] && ART="$SRC/tools_ps4/sce_sys.local"
ART="${GE_PS4_ART:-$ART}"
HOST="ftp://${PS4_HOST:-10.0.0.150}:${PS4_FTP_PORT:-2121}"
OUT="$PS4_BUILD_ROOT/out"

eboot="$BUILD/ge007_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "no eboot.bin: run tools_ps4/build.sh first" >&2; exit 1; }
[ -f "$ART/icon0.png" ] || { echo "no icon0.png in $ART (set GE_PS4_ART)" >&2; exit 1; }

prx="$SRC/build-ps4-radv/libGE64Radv.prx"
[ -f "$prx" ] || { echo "no $prx: run tools_ps4/build-radv-prx.sh" >&2; exit 1; }

stage="$BUILD/stage"
rm -rf "$stage"
mkdir -p "$stage/sce_sys" "$stage/sce_module"
cp "$eboot" "$stage/"
cp "$prx" "$stage/sce_module/"   # the Vulkan driver (port/ps4/ps4_vulkan.cpp)
cp "$ART"/*.png "$stage/sce_sys/"
echo "package art: $ART"
# Plain homebrew identity: Vulkan through RADV, no Piglet modules bundled.
# param.sfo ATTRIBUTE bit 23 ("supports NEO mode"): a PS4 Pro runs the game in
# Neo mode (36 CUs at 911 MHz; the base tiling stays valid, tested on the
# console). A base PS4 ignores it. GE_PS4_NEO=0 packages without it.
attribute=0x00800000
[ "${GE_PS4_NEO:-1}" = 0 ] && attribute=0
makepkg="$(mktemp)"
sed -e "s#^    set_entry ATTRIBUTE Integer 4 0\$#    set_entry ATTRIBUTE Integer 4 $attribute#" \
    "$HOME/DolphinPS4/scripts/make-pkg.sh" > "$makepkg"
grep -q "set_entry ATTRIBUTE Integer 4 $attribute\$" "$makepkg" \
  || { echo "make-pkg.sh changed: plain ATTRIBUTE not patched" >&2; exit 1; }
pkg="$(SFO_STYLE=plain DOLPHIN_PS4_MODULES_DIR=/nonexistent \
    bash "$makepkg" "$stage" GOLD00064 "GoldenEye 007" "$VERSION" \
    GOLDENEYE64 "$OUT" | tail -1)"
rm -f "$makepkg"
echo "param.sfo ATTRIBUTE $attribute"
echo "package: $pkg"
if [ "${2:-}" = upload ]; then
    curl -sS --max-time 300 -T "$pkg" "$HOST/data/pkg/"
    echo "uploaded $(basename "$pkg")"
    # The previous run's diagnostics (our folder only; never other apps' data).
    lftp -e "rm -f /data/GoldenEye64/ge007.log /data/GoldenEye64/crash.log /data/GoldenEye64/xmb-trace.log \
        /data/GoldenEye64/radv-ps4.log /data/GoldenEye64/samples.log; bye" "$HOST" > /dev/null 2>&1 || true
    echo "cleared old logs in /data/GoldenEye64"
fi
