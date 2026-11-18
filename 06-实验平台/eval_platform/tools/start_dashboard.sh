#!/bin/bash
# 启动 G4 实时控制台（前台运行；Ctrl-C 结束）
#   用法: bash eval_platform/tools/start_dashboard.sh [端口]
# 说明: 页面里的视频流依赖本服务做中继（RV1126B:8082 -> 浏览器 MJPEG），
#       服务没在跑时页面会显示「可见光流不可用」，并不代表板子坏了。
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ -f "$ROOT/config/lab.env" ]; then set -a; . "$ROOT/config/lab.env"; set +a; fi
PORT="${1:-${DASH_PORT:-8899}}"
PY="${PYTHON:-$(command -v python3)}"

cd "$ROOT" || exit 1

if pgrep -f "serve_g4.py --port ${PORT}" >/dev/null 2>&1; then
  echo "已在运行: http://127.0.0.1:${PORT}/g4_live.html"
  exit 0
fi

# 前置检查：RV1126B 视频服务是否活着（只提示，不阻断）
RV_API="${RV_API:-http://${RV_IP:-192.168.77.22}:${RV_VISION_PORT:-8082}}"
if ! curl -s --max-time 3 --noproxy '*' "$RV_API/status.json" >/dev/null 2>&1; then
  echo "! RV1126B:8082 暂不可达（可先跑 eval_platform/tools/check_ir_module.sh 排查）" >&2
fi

echo "控制台: http://127.0.0.1:${PORT}/g4_live.html"
exec "$PY" eval_platform/dashboard/serve_g4.py --port "$PORT"
