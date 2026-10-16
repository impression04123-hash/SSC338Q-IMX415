#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-"$root/samples/demo.h265"}
[[ ! -e "$out" ]] || { echo "Sample already exists: $out" >&2; exit 2; }
mkdir -p -- "$(dirname -- "$out")"
ffmpeg -v error -f lavfi -i testsrc2=size=320x240:rate=30 -frames:v 150 \
  -c:v libx265 -preset ultrafast \
  -x265-params 'pools=none:frame-threads=1:bframes=0:keyint=30:repeat-headers=1:log-level=error' \
  -f hevc "$out"
printf 'Created H.265 sample: %s\n' "$out"
