#!/usr/bin/env bash
# Fetch the logs from /data/GoldenEye64 (not the ROM or sidecars) into
# ~/ps4-logs/ge64-<name> and print a summary.
#   tools_ps4/logs.sh <name> [tail-lines]
set -uo pipefail
name="${1:?name}"
lines="${2:-40}"
HOST="ftp://${PS4_HOST:-10.0.0.150}:${PS4_FTP_PORT:-2121}"
dir="$HOME/ps4-logs/ge64-$name"
mkdir -p "$dir"
cd "$dir" || exit 1
rm -rf gedata
lftp -e "mirror -x data/ /data/GoldenEye64 gedata; bye" "$HOST" > /dev/null 2>&1
ls -la gedata
for f in gedata/*crash*.log gedata/*trace*.log; do
    [ -f "$f" ] && { echo "===== $f"; tail -30 "$f"; }
done
log=gedata/ge007.log
[ -f "$log" ] || { echo "no ge007.log"; exit 1; }
echo "===== $log: $(wc -l < "$log") lines"
grep -E '^\[(ERROR|WARN |FATAL)\]' "$log" | sort | uniq -c | sort -rn | head -30 | cut -c1-200
echo "===== last $lines lines"
tail -"$lines" "$log" | cut -c1-220
