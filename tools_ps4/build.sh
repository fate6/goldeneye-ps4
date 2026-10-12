#!/usr/bin/env bash
# Configure (if needed) and build the PS4 eboot with DolphinPS4's toolchain
# (clang 21 + libc++ 21 on OpenOrbis via PacBrew).
#   tools_ps4/build.sh [configure] [ninja args...]
set -uo pipefail
source "$HOME/DolphinPS4/scripts/ps4-env.sh"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${GE_PS4_BUILD:-$SRC/build-ps4}"
TC="$HOME/DolphinPS4/toolchain"
LOG="$BUILD-logs"
mkdir -p "$LOG"

if [ "${1:-}" = configure ] || [ ! -f "$BUILD/build.ninja" ]; then
    [ "${1:-}" = configure ] && shift
    rm -rf "$BUILD"
    ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TC/ps4-love-modern.cmake" \
        "-DCMAKE_PROJECT_ge007_INCLUDE=$TC/ps4-love-style.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DROMID=ntsc-final \
        -DPORT_HOST_INCLUDE_DIR="$OPENORBIS/include" \
        -DPS4_PAID=0x3800000000000035 \
        > "$LOG/configure.log" 2>&1
    rc=$?
    echo "configure rc=$rc"
    grep -E "CMake Error|Error:" -A4 "$LOG/configure.log" | head -40
    [ $rc -eq 0 ] || exit $rc
fi

ninja -C "$BUILD" -k 0 "$@" > "$LOG/build.log" 2>&1
rc=$?
echo "build rc=$rc"
grep -E "error:|undefined (symbol|reference)|check-oelf" "$LOG/build.log" | sort | uniq -c | sort -rn | head -60
tail -3 "$LOG/build.log" | cut -c1-200
exit $rc
