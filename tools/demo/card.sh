#!/usr/bin/env bash
# card.sh NAME "Title" "line 2" [SECONDS] — a title card (dark, Backpack amber) as scenes/NAME.mp4, 720x1600.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)/scenes; mkdir -p "$D"
F=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf; G=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
T=$(printf '%s' "$2" | sed "s/:/\\\\:/g; s/'/’/g"); L=$(printf '%s' "${3:-}" | sed "s/:/\\\\:/g; s/'/’/g")
ffmpeg -loglevel error -y -f lavfi -i "color=c=#14161a:s=720x1600:r=30:d=${4:-3}" \
  -vf "drawtext=fontfile=$F:text='$T':fontcolor=#e0a458:fontsize=46:x=(w-text_w)/2:y=h/2-70,drawtext=fontfile=$G:text='$L':fontcolor=#e8e6e3:fontsize=30:x=(w-text_w)/2:y=h/2+10" \
  -c:v libx264 -pix_fmt yuv420p "$D/$1.mp4"
