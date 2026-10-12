#!/usr/bin/env bash
# Upload the user's ROM and the converted sidecars to the console:
#   <data>/ge007.ntsc-final.z64, pcmodels-ntsc-final/, pccg-ntsc-final/
# -> /data/GoldenEye64/data/ (what the game resolves as $S/).
# Make the sidecars first with the three tools_pc/d*_emit.py passes (see
# docs/building.md section 4).
#   tools_ps4/upload-data.sh [data-dir]   (default: <repo>/data)
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${1:-$SRC/data}"
HOST="ftp://${PS4_HOST:-10.0.0.150}:${PS4_FTP_PORT:-2121}"
REMOTE=/data/GoldenEye64/data

for f in ge007.ntsc-final.z64 pcmodels-ntsc-final/pcmodels.bin pcmodels-ntsc-final/manifest.csv \
         pccg-ntsc-final/pccg.bin pccg-ntsc-final/manifest.csv; do
    [ -f "$DATA/$f" ] || { echo "missing $DATA/$f" >&2; exit 1; }
    curl -sS --max-time 300 --ftp-create-dirs -T "$DATA/$f" "$HOST$REMOTE/$f"
    echo "uploaded $f ($(stat -c %s "$DATA/$f") bytes)"
done
