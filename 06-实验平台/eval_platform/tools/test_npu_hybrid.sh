#!/bin/bash
# 在昇腾盒子 310B1 上验证 CANN/NPU 混合执行：
#   RMS_NORM 回退 CPU（CANN 8.0.RC3 缺该算子），其余算子（含 MUL_MAT）留在 NPU。
#
# 用法（Mac 侧）：ssh ascend 'bash -s' < eval_platform/tools/test_npu_hybrid.sh
# 不触碰生产服务 g4-llm.service（用独立端口 8090）。
set -o pipefail
BASE=/opt/g4-llm
BIN=$BASE/src/build-cann/bin/llama-server
# Q8_0 权重走 WeightQuantBatchMatmulV2 量化路径（310B 可用），
# 避免 fp16 路径的 MatMulV2 FRACTAL_NZ 内核缺失（EH9999）。
MODEL=${G4_MODEL:-$BASE/models/qwen3-0.6b-q8_0.gguf}
PORT=8090
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export ASCEND_OPP_PATH=${ASCEND_TOOLKIT_HOME}/opp
export LD_LIBRARY_PATH=$BASE/src/build-cann/bin:$LD_LIBRARY_PATH
# 三个 310B1 专属兼容开关（见 npu_attempt.md）
export GGML_CANN_NO_PINNED=1
export GGML_CANN_MEM_POOL=leg
export GGML_CANN_WEIGHT_NZ=off
export GGML_CANN_OPERATOR_FUSION=off

[ -x "$BIN" ] || { echo "[FAIL] CANN 版 llama-server 不存在，请先 build_llama_cann.sh"; exit 2; }

echo "=== 启动 NPU 混合执行服务 (port $PORT, -ngl 99, $(basename "$MODEL")) ==="
"$BIN" -m "$MODEL" --host 127.0.0.1 --port "$PORT" \
    --alias npu-hybrid -c 1024 -t 4 -ngl 99 --no-webui \
    > "$BASE/logs/npu_hybrid.out" 2>&1 &
PID=$!
code=000
for i in $(seq 1 45); do
    code=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' --max-time 2 \
        "http://127.0.0.1:$PORT/health" 2>/dev/null)
    [ "$code" = "200" ] && break
    kill -0 "$PID" 2>/dev/null || break
    sleep 1
done
echo "health=$code"
echo "--- offload / 错误关键日志 ---"
grep -iE 'offloaded|CANN0|buffer pool|GGML_ASSERT|CANN error|not implemented|does not support' \
    "$BASE/logs/npu_hybrid.out" | tail -8

if [ "$code" != "200" ]; then
    echo "[FAIL] NPU 混合执行未启动，原因见上；生产服务仍为 CPU 版，不受影响。"
    kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    exit 1
fi

echo "=== 真实 NPU 混合推理（3 次） ==="
for i in 1 2 3; do
    curl -s --noproxy '*' --max-time 120 -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
      -H 'Content-Type: application/json' \
      -d '{"model":"npu-hybrid","messages":[{"role":"user","content":"端侧检测事件判断。只回答一个词 confirm 或 reject。\n示例：标签person 置信度0.76 面积1393px 温差8.6K 持续15.1s -> confirm\n示例：标签none 置信度0.0 面积11px 温差0.2K 持续0.0s -> reject\n事件：标签person 置信度0.76 面积1393px 温差8.6K 持续15.1s ->"}],"max_tokens":8,"temperature":0}' \
      | python3 -c "import json,sys;d=json.load(sys.stdin);print('  reply:',repr(d.get('choices',[{}])[0].get('message',{}).get('content','')),'decode_tok/s:',round(d.get('timings',{}).get('predicted_per_second',0),2))" 2>&1 | head -2
done

echo "=== NPU AICore / 显存占用（推理后采样） ==="
npu-smi info 2>/dev/null | grep -E '310B1|/ 23673' | head -4

kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
echo "NPU_HYBRID_TEST_DONE"
