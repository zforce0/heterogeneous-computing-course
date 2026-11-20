#!/bin/bash
# 互联异构计算 · 控制台一键启动 / 停止 / 状态
#
#   ./start.sh                 重启控制台（清理旧进程 → 后台启动 → 健康检查 → 打开浏览器）
#   ./start.sh stop            停止控制台
#   ./start.sh status          查看运行状态
#   ./start.sh --no-open       启动但不自动打开浏览器
#   ./start.sh --rebuild       启动前重建页面（读归档 JSON，通常不需要）
#   ./start.sh <端口>          指定端口（默认 DASH_PORT 或 8899）
#
# 说明：本脚本只负责 Mac 上的显示/控制服务（127.0.0.1:PORT）。四节点实时数据
# 需要实验网接通；节点离线时页面会如实显示 offline，不代表本服务故障。
#
# 后台方式：默认用 nohup 让服务在终端关闭后继续跑（正常终端环境）。
# 若在某些受限沙箱里服务随命令结束被回收，改用前台：`python3 eval_platform/dashboard/serve_g4.py --port PORT`。

set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
if [ -f "${ROOT}/config/lab.env" ]; then set -a; . "${ROOT}/config/lab.env"; set +a; fi

ACTION="${1:-start}"
PORT="${DASH_PORT:-8899}"
# 第一个参数若是数字，视为端口；否则视为动作/选项，端口用默认值
case "${ACTION}" in
  [0-9]*) PORT="${ACTION}" ;;
esac
# 校验端口取值（只有真正当端口用时才数字；动作/选项一律放行）
if case "${ACTION}" in [0-9]*|stop|status|start|--no-open|--rebuild) false;; *) true;; esac; then
  echo "用法: ./start.sh [端口|stop|status|--no-open|--rebuild]"; exit 2
fi

PIDFILE="/tmp/g4-dashboard-${PORT}.pid"
LOGFILE="/tmp/g4-dashboard-${PORT}.log"
URL="http://127.0.0.1:${PORT}/"
PY="${PYTHON:-$(command -v python3)}"

# 找监听端口的 PID（兼容 lsof 缺失时退回 pgrep）
port_pids() {
  local pids=""
  if command -v lsof >/dev/null 2>&1; then
    pids="$(lsof -nP -iTCP:${PORT} -sTCP:LISTEN -t 2>/dev/null | tr '\n' ' ')"
  fi
  if [ -z "${pids}" ] && command -v pgrep >/dev/null 2>&1; then
    pids="$(pgrep -f "serve_g4.py --port ${PORT}" 2>/dev/null | tr '\n' ' ')"
  fi
  echo "${pids}"
}

health() {
  curl -s -m 2 --noproxy '*' -o /dev/null -w '%{http_code}' "${URL}api/state" 2>/dev/null
}

open_browser() {
  case "$(uname -s)" in
    Darwin) open "${URL}" >/dev/null 2>&1 ;;
    Linux)  command -v xdg-open >/dev/null 2>&1 && xdg-open "${URL}" >/dev/null 2>&1 ;;
  esac
}

node_diag() {
  echo "── 节点连通性诊断（离线多半是实验网未接，不是本服务问题）"
  local RV="${RV_API:-http://${RV_IP:-192.168.77.22}:${RV_VISION_PORT:-8082}}"
  local ASC="http://${ASCEND_IP:-192.168.77.10}:${LLM_PORT:-8080}"
  for item in "RV1126B 视觉 ${RV}/status.json" "昇腾 LLM ${ASC}/health"; do
    local name=$(echo "$item" | cut -d' ' -f1)
    local u=$(echo "$item" | cut -d' ' -f2-)
    local c=$(curl -s -m 3 --noproxy '*' -o /dev/null -w '%{http_code}' "${u}" 2>/dev/null)
    [ "${c}" = "200" ] && echo "  [在线] ${name}" || echo "  [离线] ${name} (HTTP ${c:-000})"
  done
}

case "${1:-start}" in
  stop)
    echo "停止控制台（端口 ${PORT}）…"
    pids="$(port_pids)"
    if [ -n "${pids}" ]; then
      kill ${pids} 2>/dev/null || true
      for i in $(seq 1 10); do [ -z "$(port_pids)" ] && break; sleep 0.5; done
    fi
    [ -f "${PIDFILE}" ] && rm -f "${PIDFILE}"
    [ -z "$(port_pids)" ] && echo "✓ 已停止" || echo "! 仍有进程占用端口: $(port_pids)"
    ;;
  status)
    code=$(health)
    pids="$(port_pids)"
    echo "控制台状态（端口 ${PORT}）"
    echo "  进程 PID: ${pids:-无}"
    echo "  /api/state: HTTP ${code:-000}"
    if [ -f "${LOGFILE}" ]; then
      echo "  日志尾部: ${LOGFILE}"
      tail -5 "${LOGFILE}" | sed 's/^/    /'
    fi
    ;;
  start|--no-open|--rebuild|[0-9]*)
    # 1) 清理旧进程（保证"一键"= 干净的重启）
    pids="$(port_pids)"
    if [ -n "${pids}" ]; then
      echo "清理旧进程: ${pids}"
      kill ${pids} 2>/dev/null || true
      for i in $(seq 1 10); do [ -z "$(port_pids)" ] && break; sleep 0.5; done
    fi

    # 2) 可选重建页面
    if echo " $* " | grep -q -- '--rebuild'; then
      echo "重建页面…"
      ( cd "${ROOT}" && "${PY}" eval_platform/dashboard/build_g4_dashboard.py ) || echo "! 页面重建失败，继续用现有页面"
    fi

    # 3) 后台启动（nohup 使服务脱离本脚本持续运行）
    echo "启动服务（端口 ${PORT}）…"
    ( cd "${ROOT}" && nohup "${PY}" eval_platform/dashboard/serve_g4.py --port "${PORT}" > "${LOGFILE}" 2>&1 & echo $! > "${PIDFILE}" )
    NEWPID=$(cat "${PIDFILE}" 2>/dev/null)

    # 4) 健康检查：等待 /api/state = 200
    ok=0
    for i in $(seq 1 20); do
      c=$(health)
      [ "${c}" = "200" ] && { ok=1; break; }
      sleep 1
    done
    if [ "$ok" != "1" ]; then
      echo "! 服务未就绪，查看日志: ${LOGFILE}"
      tail -15 "${LOGFILE}" 2>/dev/null | sed 's/^/    /'
      exit 1
    fi

    echo "✓ 控制台就绪: ${URL}  (PID ${NEWPID:-?})"
    node_diag

    # 5) 打开浏览器（除非 --no-open）
    if ! echo " $* " | grep -q -- '--no-open'; then
      open_browser
      echo "已尝试打开浏览器。若未自动打开，请手动访问: ${URL}"
    fi
    echo "日志: ${LOGFILE}   |   停止: ./start.sh stop"
    ;;
  *)
    echo "用法: ./start.sh [端口|stop|status|--no-open|--rebuild]"
    exit 2
    ;;
esac
