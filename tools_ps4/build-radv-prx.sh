#!/usr/bin/env bash
# Link Mesa's RADV (tools_ps4/build-mesa.sh -> build/mesa-ge64) into a PS4
# library module, libGE64Radv.prx, loaded by the game at startup
# (port/ps4/ps4_vulkan.cpp).
#
# Why a separate module: linked into the eboot, Mesa's ~26 MB of code and
# read-only data push the game's own data above 0x01000000, where fast3d's
# seg_addr() reads addresses as N64 segment references (segments 1 and 2 are
# in use) - see port/include/portaddr.h. The eboot always loads at 0x400000.
#   tools_ps4/build-radv-prx.sh [out-dir]
set -euo pipefail
source "$HOME/DolphinPS4/scripts/ps4-env.sh"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="${1:-$SRC/build-ps4-radv}"
MESA="${GE_PS4_MESA_BUILD:-$PS4_BUILD_ROOT/build/mesa-ge64}"
SYSROOT="${PS4_SYSROOT:-$PS4_BUILD_ROOT/sysroot}"
mkdir -p "$OUTDIR"

libs=(src/amd/vulkan/libvulkan_radeon.a src/amd/common/libamd_common.a
      src/amd/compiler/libaco.a src/amd/addrlib/libaddrlib.a src/vulkan/util/libvulkan_util.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libparson.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
whole=()
for l in "${libs[@]}"; do
    [ -f "$MESA/$l" ] || { echo "missing $MESA/$l: run tools_ps4/build-mesa.sh setup" >&2; exit 1; }
    whole+=("$MESA/$l")
done

# OpenOrbis' link.x only collects plain .data/.tdata/.tbss and .init_array: the
# per-symbol sections of Mesa's -fdata-sections build became orphans past the
# end of the data segment's file image, i.e. ALL of Mesa's initialised data
# started out as zeros (and prioritised constructors never ran). Same fix as
# DolphinPS4's ps4-love-style.cmake applies for eboots.
# crtlib.o's module_start runs the constructors between its OWN (empty)
# __init_array_start/_end, never the module's .init_array (which holds ACO's
# opcode table setup: without it every compiled shader is garbage). Mark the
# real array; ge64_radv_set_heap() runs it (radv_prx_exports.c).
sed -e 's/\*(\.init_array);/__ge64_init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))); KEEP(*(.init_array)); __ge64_init_array_end = .;/' \
    -e 's/\*(\.data)/*(.data .data.*)/' -e 's/\*(\.tdata)/*(.tdata .tdata.*)/' -e 's/\*(\.tbss)/*(.tbss .tbss.*)/' \
    "$OPENORBIS/link.x" > "$OUTDIR/link.x"
for want in 'SORT_BY_INIT_PRIORITY' '__ge64_init_array_end' '\.data \.data\.\*' '\.tdata \.tdata\.\*' '\.tbss \.tbss\.\*'; do
    grep -q "$want" "$OUTDIR/link.x" || { echo "link.x layout changed: '$want' not patched" >&2; exit 1; }
done

clang-21 -target x86_64-pc-freebsd12-elf -fPIC -O2 -isysroot "$OPENORBIS" -isystem "$OPENORBIS/include" \
    -c "$SRC/tools_ps4/radv_prx_exports.c" -o "$OUTDIR/radv_prx_exports.o"
rm -f "$OUTDIR/libGE64Radv.elf"
"$OPENORBIS/bin/ld.lld" -m elf_x86_64 -pie --script "$OUTDIR/link.x" --eh-frame-hdr \
    -L"$OPENORBIS/lib" -o "$OUTDIR/libGE64Radv.elf" \
    --export-dynamic-symbol=vk_icdGetInstanceProcAddr --export-dynamic-symbol=ge64_radv_submit_flip \
    --export-dynamic-symbol=ge64_radv_set_heap \
    --wrap=malloc --wrap=free --wrap=calloc --wrap=realloc --wrap=memalign --wrap=__memalign \
    --wrap=pthread_once --wrap=getenv \
    "$OPENORBIS/lib/crtlib.o" "$OUTDIR/radv_prx_exports.o" \
    --whole-archive "${whole[@]}" --no-whole-archive \
    "$OPENORBIS/usr/lib/libz.a" "$SYSROOT/lib/libc++.a" "$SYSROOT/lib/libc++abi.a" "$OPENORBIS/lib/libunwind.a" \
    -lkernel -lc -lclang_rt.builtins-x86_64 -lSceLibcInternal -lSceGnmDriver -lSceVideoOut \
    "$@" 2>&1 | tee "$OUTDIR/link.log" | grep -E "error|undefined" | sort | uniq -c | sort -rn | head -40 || true
[ -f "$OUTDIR/libGE64Radv.elf" ] || { echo "link failed (see $OUTDIR/link.log)" >&2; exit 1; }
( cd "$OUTDIR" && OO_PS4_TOOLCHAIN="$OPENORBIS" "$OPENORBIS/bin/create-fself" \
    -in=libGE64Radv.elf -out=libGE64Radv.oelf --lib=libGE64Radv.prx --paid 0x3800000000000011 )
ls -la "$OUTDIR/libGE64Radv.prx"
