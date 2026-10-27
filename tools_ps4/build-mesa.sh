#!/usr/bin/env bash
# Build the GoldenEye 64 variant of DolphinPS4's RADV-on-GNM Mesa into
# ~/dolphinps4-build/build/mesa-ge64 (from the x360 port's build-mesa-ge.sh):
#  - the PS4 winsys (port/mesa/ac_ps4_drm.c) logs to /data/GoldenEye64;
#  - fence waits set errno like the DRM ioctl (else every "not done yet" poll
#    became VK_ERROR_UNKNOWN);
#  - geometry-shader rings >= 16 MiB (the system owns the ring size registers;
#    unused by this renderer, kept so the build matches the tested Mesa);
#  - PS4 Pro Neo mode detection: clock and log, and the raster config
#    experiment GE_RASTER=0/1/2 (tools_ps4/mesa-neo.patch, ac_gpu_info.c).
# The Mesa source tree is shared with DolphinPS4's build; each run puts the
# right ac_ps4_drm.c in place, so run DolphinPS4's build-mesa.sh again before
# DolphinPS4 work.
#   tools_ps4/build-mesa.sh [setup]
set -euo pipefail
ROOT="$HOME/dolphinps4-build"
MESA="$ROOT/src/mesa"
SRC="$HOME/DolphinPS4/port/mesa/ac_ps4_drm.c"
OUT="$ROOT/build/mesa-ge64"

script="$(mktemp)"
sed -e "s#build=\"\$ROOT/build/mesa\"#build=\"$OUT\"#" \
    -e 's#^tr -d .*port/mesa/ac_ps4_drm\.c.*$#true  \# ac_ps4_drm.c put in place by tools_ps4/build-mesa.sh#' \
    "$HOME/DolphinPS4/scripts/build-mesa.sh" > "$script"
grep -q "$OUT" "$script" || { echo "build-mesa.sh layout changed: build dir not patched" >&2; exit 1; }
grep -q "put in place by tools_ps4/build-mesa.sh" "$script" \
  || { echo "build-mesa.sh layout changed: ac_ps4_drm.c copy not replaced" >&2; exit 1; }

tr -d '\015' < "$SRC" | sed -e 's#/data/DolphinPS4/#/data/GoldenEye64/#g' \
  > "$MESA/src/amd/common/ac_ps4_drm.c"

perl -0pi -e '
  s/   return ps4_wait_common\(\(ac_drm_device \*\)p, handles, NULL, count, timeout_nsec, flags, first\);\n/   const int r = ps4_wait_common((ac_drm_device *)p, handles, NULL, count, timeout_nsec, flags, first);\n   if (r < 0)\n      errno = -r;\n   return r;\n/;
  s/   return ps4_wait_common\(\(ac_drm_device \*\)p, handles, points, count, timeout_nsec, flags, first\);\n/   const int r = ps4_wait_common((ac_drm_device *)p, handles, points, count, timeout_nsec, flags, first);\n   if (r < 0)\n      errno = -r;\n   return r;\n/;
' "$MESA/src/amd/common/ac_ps4_drm.c"
[ "$(grep -c 'errno = -r;' "$MESA/src/amd/common/ac_ps4_drm.c")" -ge 2 ] \
  || { echo "ac_ps4_drm.c changed: wait errno fix not patched" >&2; exit 1; }

# PS4 Pro Neo mode: detected at run time (sceKernelIsNeoMode); the base tiling stays.
patch --quiet --forward "$MESA/src/amd/common/ac_ps4_drm.c" < "$(dirname "$0")/mesa-neo.patch" \
  || { echo "ac_ps4_drm.c changed: mesa-neo.patch does not apply" >&2; exit 1; }

INFO="$MESA/src/amd/vulkan/radv_shader_info.c"
if ! grep -q "GE PS4 GS ring minimum" "$INFO"; then
  perl -0pi -e '
    s/(   out->gsvs_ring_size = MIN2\(gsvs_ring_size, max_size\);\n)/$1   \/\* GE PS4 GS ring minimum: the hardware ring size is the system one, not ours. \*\/\n   out->esgs_ring_size = MAX2(out->esgs_ring_size, 16u << 20);\n   out->gsvs_ring_size = MAX2(out->gsvs_ring_size, 16u << 20);\n/;
  ' "$INFO"
fi
[ "$(grep -c 'GE PS4 GS ring minimum' "$INFO")" -eq 1 ] \
  || { echo "radv_shader_info.c changed: GS ring minimum not patched" >&2; exit 1; }

# Raster config experiment (GE_RASTER, mesa-neo.patch): the winsys picks it;
# the weak default keeps DolphinPS4's build (same Mesa tree) on Bonaire's.
GPU="$MESA/src/amd/common/ac_gpu_info.c"
if ! grep -q "GE PS4 raster" "$GPU"; then
  perl -0pi -e '
    s/(void ac_get_raster_config\(const struct radeon_info \*info)/\/\* GE PS4 raster: 0 = Mesa default; the GoldenEye 64 winsys (ac_ps4_drm.c) overrides this. \*\/\nint ac_ps4_raster_mode(void);\n__attribute__((weak)) int ac_ps4_raster_mode(void)\n{\n   return 0;\n}\n\n$1/;
    s/(   \/\* drm\/radeon on Kaveri is buggy)/   if (info->family == CHIP_BONAIRE && ac_ps4_raster_mode() >= 2) { \/\* GE PS4 raster: Neo, 4 SEs \/ 16 RBs (as Hawaii) \*\/\n      raster_config = 0x3a00161a;\n      raster_config_1 = 0x0000002e;\n   } else if (info->family == CHIP_BONAIRE && ac_ps4_raster_mode() == 1) { \/\* GE PS4 raster: 2 SEs \/ 8 RBs (as Tahiti) \*\/\n      raster_config = 0x2a00126a;\n      raster_config_1 = 0x00000000;\n   }\n\n$1/;
  ' "$GPU"
fi
[ "$(grep -c 'GE PS4 raster' "$GPU")" -eq 3 ] \
  || { echo "ac_gpu_info.c changed: raster config experiment not patched" >&2; exit 1; }

HERE_DIR="$HOME/DolphinPS4/scripts"
cp "$script" "$HERE_DIR/.build-mesa-ge64.tmp.sh"
chmod +x "$HERE_DIR/.build-mesa-ge64.tmp.sh"
"$HERE_DIR/.build-mesa-ge64.tmp.sh" "${1:-}"
rm -f "$HERE_DIR/.build-mesa-ge64.tmp.sh" "$script"
echo "mesa-ge64 built in $OUT"
