#!/bin/bash
# G4 新架构总编排：Mac 仅显示/控制；昇腾盒子=中心 LLM；RK3588 / RV1126B=端侧视觉。
# 用法: bash run_g4_pipeline.sh check | llm | rk | rv | all
# 原则：先做连通性前置检查，不通就明确拒绝，绝不伪造"已运行"结果。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ -f "$ROOT/config/lab.env" ]; then set -a; . "$ROOT/config/lab.env"; set +a; fi
ASCEND="${ASCEND_IP:-192.168.77.10}"
RK="${RK_IP:-192.168.77.21}"
RV="${RV_IP:-192.168.77.22}"
LLM_API="http://$ASCEND:${LLM_PORT:-8080}/v1/chat/completions"

alive(){ ping -c 1 -t 2 "$1" 2>&1 | grep -q round-trip; }
ssh_ok(){ ssh -o BatchMode=yes -o ConnectTimeout=4 "$1" true >/dev/null 2>&1; }
llm_ok(){ curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://$ASCEND:${LLM_PORT:-8080}/health" 2>/dev/null | grep -q 200; }

check(){
  echo "=== G4 链路前置检查 ==="
  local ok=0
  alive "$ASCEND" && echo "[PASS] 昇腾盒子 $ASCEND 可达" || { echo "[FAIL] 昇腾盒子不可达"; ok=1; }
  ssh_ok ascend    && echo "[PASS] ascend SSH 免密" || { echo "[WARN] ascend SSH 免密未配"; }
  llm_ok           && echo "[PASS] 中心 LLM :8080/health = 200" || { echo "[FAIL] 中心 LLM 服务未就绪（ssh ascend 'systemctl status g4-llm'）"; ok=1; }
  alive "$RK" && echo "[PASS] RK3588 $RK 可达" || { echo "[FAIL] RK3588 不可达"; ok=1; }
  ssh_ok rk3588    && echo "[PASS] rk3588 SSH 免密" || echo "[WARN] rk3588 SSH 免密未配"
  alive "$RV" && echo "[PASS] RV1126B $RV 可达" || echo "[WAIT] RV1126B 未上电/未接线（archived 关键帧仍可展示）"
  echo "LLM API: $LLM_API"
  return $ok
}

run_llm(){
  echo "=== [LLM] Mac 侧实测昇腾盒子推理吞吐 ==="
  python3 "$ROOT/eval_platform/tools/bench_llm_ascend.py" || return 1
  echo "=== [LLM] 用昇腾 LLM 重算 4 组带时间关键帧 ==="
  python3 "$ROOT/eval_platform/tools/build_keyframes_ascend.py"
}

run_rk(){
  echo "=== [RK3588] 端侧视觉事件 -> 昇腾 LLM 语义二次确认 ==="
  ssh -o BatchMode=yes rk3588 'python3 /home/cat/eval_dashboard/rk_llm_reconfirm.py' || return 1
  echo "--- 回收结果 ---"
  ssh -o BatchMode=yes rk3588 'cat /home/cat/eval_dashboard/llm_reconfirm.json' | head -30
}

run_rv(){
  echo "=== [RV1126B] 三摄采集 + YOLOv8n (RKNN) 端侧推理 ==="
  if ! alive "$RV" || ! ssh_ok rv1126b; then
    echo "[WAIT] RV1126B 当前不在线或 SSH 免密未配 —— 跳过，不伪造 fresh 帧。"
    echo "       上电后: bash eval_platform/tools/finalize_lab_setup.sh 装公钥，再跑本脚本 rv。"
    return 2
  fi
  ssh rv1126b 'systemctl stop camera-preview.service 2>/dev/null || true' || true
  echo "[RV1126B] 启动 dual_demo（20s 短测）"
  ssh rv1126b 'cd /userdata/dual_demo && timeout 20 ./run.sh --mode auto --card inspect' || true
  mkdir -p "$ROOT/eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/frames/rv1126b"
  scp -q rv1126b:'/userdata/dual_demo/output/det_*_vis.jpg /userdata/dual_demo/output/det_*_ir.jpg' \
      "$ROOT/eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/frames/rv1126b/" 2>/dev/null || true
  echo "[RV1126B] 事件送昇腾 LLM 复核（板端无 python requests，走 curl）"
  ssh rv1126b "curl -s -X POST '$LLM_API' -H 'Content-Type: application/json' -d '{\"model\":\"g4-edge-llm\",\"messages\":[{\"role\":\"user\",\"content\":\"ping from rv1126b\"}],\"max_tokens\":32}' | head -c 300" || true
}

ACTION="${1:-check}"
case "$ACTION" in
  check) check ;;
  llm)   check && run_llm ;;
  rk)    check && run_rk ;;
  rv)    run_rv ;;
  all)   check && run_llm && run_rk; run_rv ;;
  *) echo "用法: $0 check|llm|rk|rv|all"; exit 64 ;;
esac
