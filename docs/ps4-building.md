# Building the PS4 version

This guide goes from a fresh Ubuntu to an installed package. Everything runs in
**Linux or WSL 2 with Ubuntu 24.04** (the tested setup). The build itself needs
no ROM; your ROM is only used for the data files in step 8.

What gets built:

| Piece | From | Result |
|---|---|---|
| PS4 C++ runtime (libc++ 21) | DolphinPS4's toolchain scripts | `~/dolphinps4-build/sysroot` |
| Vulkan driver (Mesa RADV on GNM) | Mesa 26.0.8 + DolphinPS4's patch + this repo's | `build-ps4-radv/libGE64Radv.prx` |
| The game | this repo | `build-ps4/ge007_eboot/eboot.bin` |
| The package | the two above + `tools_ps4/sce_sys` | `IV0000-GOLD00064_00-GOLDENEYE6400000.pkg` |

The first build takes a while (libc++ and Mesa); after that, rebuilding the game
takes a minute or two.

## 1. Host packages

clang 21 comes from [apt.llvm.org](https://apt.llvm.org):

```sh
wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 21
sudo apt install build-essential cmake ninja-build pkg-config git curl lftp \
    python3 python3-venv python3-pil glslang-tools
```

Check: `clang-21 --version`, `ld.lld-21 --version` and `glslangValidator --version` all work.

## 2. The OpenOrbis toolchain (PacBrew)

Install [PacBrew](https://github.com/PacBrew)'s PS4 toolchain so that it lands in
`/opt/pacbrew/ps4/openorbis` (set `OPENORBIS` if yours is elsewhere). The
packages this build uses:

```sh
sudo pacman -S ps4-openorbis-headers ps4-openorbis-musl ps4-openorbis-libcxx \
    ps4-openorbis-vars ps4-openorbis-clang ps4-openorbis-create-fself \
    ps4-openorbis-pkgtools ps4-openorbis-sdl2 ps4-openorbis-libsamplerate \
    ps4-openorbis-zlib
```

(`pacman` here is PacBrew's, configured with its package server; follow PacBrew's
own install instructions for that part.)

Check: `/opt/pacbrew/ps4/openorbis/ps4vars.sh` and
`/opt/pacbrew/ps4/openorbis/bin/linux/` exist.

## 3. DolphinPS4 at the tested commit

[DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) provides the toolchain
scripts, the Mesa patch and the GNM winsys. The scripts here look for it in
`~/DolphinPS4` (set `DOLPHINPS4` for another place) and warn if it is not at the
tested commit:

```sh
git clone https://github.com/iHaiDeeZ/DolphinPS4 ~/DolphinPS4
git -C ~/DolphinPS4 checkout 6297f9d5889f87fe657a3dea784ff8c6b40ed63b
```

The build tree lives in `~/dolphinps4-build` (set `PSB` / `PS4_BUILD_ROOT` to move it).

## 4. The PS4 C++ runtime

```sh
cd ~/DolphinPS4
toolchain/build-libcxx.sh      # downloads LLVM 21.1.6's runtimes and builds libc++ for the PS4
toolchain/build-runtime.sh
```

Check: `~/dolphinps4-build/sysroot/lib/libc++.a` exists.

## 5. Mesa and libdrm sources

Mesa 26.0.8 with DolphinPS4's patch, and the libdrm 2.4.125 headers (the PS4 has
no DRM; only the headers are used):

```sh
mkdir -p ~/dolphinps4-build/src && cd ~/dolphinps4-build/src
git clone --depth 1 --branch mesa-26.0.8 https://gitlab.freedesktop.org/mesa/mesa.git mesa
git -C mesa apply ~/DolphinPS4/patches/mesa-ps4.patch
git clone --depth 1 --branch libdrm-2.4.125 https://gitlab.freedesktop.org/mesa/libdrm.git libdrm
```

Mesa's build tools go in a virtual environment the scripts put on `PATH`:

```sh
python3 -m venv ~/dolphinps4-build/mesa-venv
~/dolphinps4-build/mesa-venv/bin/pip install meson mako pyyaml packaging
```

## 6. This repository

```sh
git clone https://github.com/fate6/goldeneye-ps4 ~/goldeneye-ps4
cd ~/goldeneye-ps4
```

Optional, for the upload scripts: put your console's address in
`tools_ps4/local.env` (gitignored):

```sh
echo PS4_HOST=192.168.1.50 > tools_ps4/local.env    # PS4_FTP_PORT defaults to 2121
```

## 7. Build and package

```sh
bash tools_ps4/build-mesa.sh setup     # RADV with this game's patches (later runs: without "setup")
bash tools_ps4/build-radv-prx.sh       # -> build-ps4-radv/libGE64Radv.prx
bash tools_ps4/build.sh                # -> build-ps4/ge007_eboot/eboot.bin
bash tools_ps4/package.sh 01.00        # -> ~/dolphinps4-build/out/IV0000-GOLD00064_00-GOLDENEYE6400000.pkg
```

- `package.sh <version> upload` also copies the package to the console's
  `/data/pkg/` over FTP.
- `build-mesa.sh` patches the shared Mesa tree in `~/dolphinps4-build/src/mesa`.
  If you also build DolphinPS4 from the same tree, run its `scripts/build-mesa.sh`
  again before that.
- `build.sh configure` reconfigures from scratch; the logs are in `build-ps4-logs/`.
- Package art (home screen icon and background): see
  [Changing the package art](../PS4.md#changing-the-package-art).

## 8. Game data from your ROM

On any PC with Python 3 (no other dependencies), in this repository:

```sh
mkdir -p data
cp /path/to/your/rom.z64 data/ge007.ntsc-final.z64      # NTSC-U ROM
python3 tools_pc/d43_emit.py ntsc-final
python3 tools_pc/d69_emit.py ntsc-final
python3 tools_pc/d88_emit.py ntsc-final --regen          # after d69, it appends to its output
```

Then copy them to the console, either with `bash tools_ps4/upload-data.sh` or by
hand over FTP:

```
/data/GoldenEye64/data/ge007.ntsc-final.z64
/data/GoldenEye64/data/pcmodels-ntsc-final/pcmodels.bin, manifest.csv
/data/GoldenEye64/data/pccg-ntsc-final/pccg.bin, manifest.csv
```

`data/` is gitignored: never commit or share these files.

## 9. Install and run

1. Start GoldHEN, install the package with its package installer and launch
   **GoldenEye 007**.
2. Settings, saves and logs are in `/data/GoldenEye64/`. `bash tools_ps4/logs.sh <name>`
   fetches the logs into `~/ps4-logs/ge64-<name>/`.
3. `/data/GoldenEye64/env.txt` (one `NAME=value` per line) sets diagnostic
   switches, for example `GE_VKDEBUG=1` for more renderer logging. Leave it empty
   normally.

If the game shows the intro logos and then stops, the data files from step 8 are
missing or in the wrong folder.
