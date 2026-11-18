#!/bin/bash
# 在昇腾盒子 (192.168.77.10) 上安装并启动 G4 中心 LLM 服务。
# Mac 只做显示/控制，不再承担任何 LLM 推理。
# 用法（Mac 侧）：ssh ascend 'bash -s' < eval_platform/tools/ascend_llm_service.sh
set -e
BASE=/opt/g4-llm
MODEL=$BASE/models/qwen3-0.6b-q4.gguf
PORT=${G4_PORT:-8080}
NGL=${G4_NGL:-0}          # CANN 构建成功时改成 99 即可启用 NPU offload

# 生产服务固定使用 CPU(ARM NEON) 版：CANN/NPU 版虽能编译并枚举到 CANN0，
# 但 CANN 8.0.RC3 在 310B1 上缺 RmsNorm 算子，运行时 abort（见 npu_attempt.md）。
BIN=$(find "$BASE/src/build-cpu" -name llama-server -type f 2>/dev/null | head -1)
if [ -z "$BIN" ]; then
    BIN=$(find "$BASE/src" -path '*/bin/llama-server' -type f 2>/dev/null | head -1)
fi
if [ -z "$BIN" ]; then
    echo "!!! 未找到编译产物 llama-server，请先运行 build_llama_cann.sh"
    exit 2
fi
BINDIR=$(dirname "$BIN")
echo "[1/4] llama-server: $BIN"

cat > "$BASE/llama_server.sh" <<EOF
#!/bin/bash
# G4 中心 LLM · 昇腾盒子 · OpenAI 兼容 API
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export LD_LIBRARY_PATH="$BINDIR:\$LD_LIBRARY_PATH"
exec "$BIN" \\
    -m "$MODEL" \\
    --host 0.0.0.0 --port $PORT \\
    --alias g4-edge-llm \\
    -c 2048 -t 4 -ngl $NGL \\
    --no-webui \\
    > "$BASE/logs/llama_server.out" 2>&1
EOF
chmod +x "$BASE/llama_server.sh"

echo "[2/4] 写 systemd unit"
cat > /etc/systemd/system/g4-llm.service <<'EOF'
[Unit]
Description=G4 Central LLM (llama.cpp) on Ascend 310B1
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
ExecStart=/opt/g4-llm/llama_server.sh
Restart=always
RestartSec=3
StandardOutput=null
StandardError=null

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable g4-llm.service >/dev/null 2>&1 || true
systemctl restart g4-llm.service

echo "[3/4] 等待服务就绪"
for i in $(seq 1 30); do
    code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$PORT/health" 2>/dev/null || true)
    [ "$code" = "200" ] && break
    sleep 2
done
echo "health=http://127.0.0.1:$PORT/health -> ${code:-000}"
systemctl is-active g4-llm.service

echo "[4/4] 从局域网侧确认"
curl -s --max-time 3 "http://127.0.0.1:$PORT/v1/models" | head -c 400 || true
echo
echo "G4_LLM_SERVICE=READY port=$PORT ngl=$NGL bin=$BIN"
