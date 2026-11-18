#!/bin/bash
# 屏幕录制（截图合成版）：激活 Chrome 保持前台 → screencapture 连续抓帧 → ffmpeg 合成。
# 注意：不要 open 新 URL（新 tab 加载慢、MJPEG 流建立前画面变化少）；
# 只激活已加载好的控制台 tab，画面即持续刷新。
# 用法: bash scripts/record_screen.sh <输出.mp4> <时长秒> [fps=3]
set -u
OUT="$1"; DUR="${2:-15}"; FPS="${3:-3}"
APP="Google Chrome"
WORK="$(mktemp -d /tmp/rec.XXXXXX)"
INTERVAL="$(python3 -c "print(round(1/$FPS,3))")"

open -a "$APP"
sleep 1

N=0
START="$(python3 -c "import time;print(time.time())")"
while :; do
  NOW="$(python3 -c "import time;print(time.time())")"
  [ "$(python3 -c "print($NOW - $START >= $DUR)")" = "True" ] && break
  N=$((N+1))
  screencapture -x -t png "$WORK/f$(printf %05d "$N").png" 2>/dev/null
  sleep "$INTERVAL"
done
if [ "$N" -eq 0 ]; then echo "无帧"; exit 1; fi
ffmpeg -y -loglevel error -framerate "$FPS" -i "$WORK/f%05d.png" \
  -vf "scale=1280:800" -c:v libx264 -preset veryfast -crf 28 -pix_fmt yuv420p "$OUT"
echo "录制完成: $OUT (${N} 帧, ~${DUR}s)"
rm -rf "$WORK"
