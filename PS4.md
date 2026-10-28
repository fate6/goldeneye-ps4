# GoldenEye 007 on PS4

A PS4 homebrew build of the [GoldenEye 007 PC port](README.md): the game's
reconstructed C from the [decompilation](https://github.com/n64decomp/007),
compiled for the PS4 and drawn through Vulkan on Mesa's RADV driver. It runs on
a jailbroken console with [GoldHEN](https://github.com/GoldHEN/GoldHEN).

> [!IMPORTANT]
> **You must supply your own GoldenEye 007 ROM** (NTSC-U, `ntsc-final`). This
> repository and its packages contain no Nintendo code or assets, no ROM, and no
> Sony SDK files or system modules.

## Status

Tested on a PS4 Pro, firmware 9.00, GoldHEN.

- The campaign plays end to end at 60 fps; saves, rumble, audio and 2-4 player
  split-screen work.
- PS4 Pro: runs in Neo mode. Render resolution 1080p, 1440p (default on a Pro,
  holds 60 fps) or 2160p (dips into the 40s in heavy scenes), set in the options
  menu. On a 4K TV the output is native 3840x2160.
- Not yet tested: a base PS4, a second physical controller in split-screen.
- Known limit: the renderer uses 4 of the PS4 Pro's 16 render backends.

## Controls (DualShock 4)

| | |
|---|---|
| Fire | R2 |
| Aim | L2 |
| Crouch | L1 |
| Pause | OPTIONS |
| Options menu | Touchpad |

Everything can be rebound in the options menu (Layout presets > Custom).

## Install

1. Build or obtain `IV0000-GOLD00064_00-GOLDENEYE6400000.pkg` and install it with
   GoldHEN's package installer.
2. Make the data files from your ROM on a PC (Python 3 only): see
   [docs/ps4-building.md](docs/ps4-building.md#8-game-data-from-your-rom).
3. Copy them to the console under `/data/GoldenEye64/data/`:
   ```
   ge007.ntsc-final.z64
   pcmodels-ntsc-final/pcmodels.bin, manifest.csv
   pccg-ntsc-final/pccg.bin, manifest.csv
   ```
   `tools_ps4/upload-data.sh` does this over GoldHEN's FTP server.

Settings, saves and logs live in `/data/GoldenEye64/` (`ge007.ini`, `ge007.log`).

## Building

Linux or WSL 2 (Ubuntu 24.04). In short: clang 21, PacBrew's OpenOrbis toolchain,
[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) at commit `6297f9d` for the
C++ runtime and the Mesa patch, then:

```
bash tools_ps4/build-mesa.sh setup
bash tools_ps4/build-radv-prx.sh
bash tools_ps4/build.sh
bash tools_ps4/package.sh 01.00
```

The full step-by-step guide, from a fresh Ubuntu to an installed package, is
**[docs/ps4-building.md](docs/ps4-building.md)**.

## How it works

- `port/ps4/`: the platform layer (VideoOut presentation, pads, audio, system
  UI handling) and `port/fast3d/gfx_vulkan.cpp`, a Vulkan renderer for the port's
  software RSP.
- Mesa's RADV runs on Sony's GNM kernel interface through DolphinPS4's winsys,
  built as a separate module (`libGE64Radv.prx`) that the game loads at start-up.
- PS4 Pro notes (Neo mode, the tiled 4K display layout measured on the console):
  see the comments in `tools_ps4/mesa-neo.patch`, `port/ps4/ps4_vulkan.cpp` and
  `port/fast3d/shaders/gfx_vulkan_tile.comp`.

## Credits and licences

- PC port: [jkdansereau/goldeneye-pc-port](https://github.com/jkdansereau/goldeneye-pc-port), MIT.
  The PS4 additions are MIT too. Third-party code keeps its own licence (see [NOTICE](NOTICE)).
- Decompilation: [n64decomp/007](https://github.com/n64decomp/007).
- [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) by iHaiDeeZ: the RADV-on-GNM
  winsys (`ac_ps4_drm.c`, MIT) and Mesa patches compiled into `libGE64Radv.prx`, and
  the build and packaging scripts (GPL-2.0, used as tools, not shipped).
- [Mesa](https://mesa3d.org) (RADV): MIT and per-file licences.
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) and
  [PacBrew](https://github.com/PacBrew): toolchain and packaging tools.
- [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator): MIT.
- PS4 Pro research: [shadPS4](https://github.com/shadps4-emu/shadPS4) and
  [OpenGNM](https://github.com/PS4-OpenGNM/opengnm) documentation of Neo tiling and register
  layouts; [psdevwiki](https://www.psdevwiki.com/ps4/) for `param.sfo`.

GoldenEye 007 is a trademark of its owners. This project is not affiliated with
or endorsed by Nintendo, Rare, MGM, Danjaq or Sony Interactive Entertainment.
