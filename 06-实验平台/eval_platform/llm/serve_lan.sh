#!/usr/bin/env bash
# 把 M1 中心 LLM 暴露到实验以太网，供 RK3588 / RV1126B / 昇腾盒子调用。
# 刻意使用 11435 端口，避免与本机常驻 Ollama(127.0.0.1:11434) 冲突。
set -euo pipefail

LAB_IFACE="${LAB_IFACE:-en5}"
LAB_IP="${LAB_IP:-$(ipconfig getifaddr "$LAB_IFACE" 2>/dev/null || true)}"
LAB_PORT="${LAB_PORT:-11435}"

if [[ -z "$LAB_IP" ]]; then
  echo "ERROR: $LAB_IFACE 没有 IPv4 地址。先配置静态地址："
  echo "  sudo networksetup -setmanual \"AX88179B\" 192.168.77.1 255.255.255.0"
  exit 1
fi

echo "中心 LLM 节点: http://${LAB_IP}:${LAB_PORT}  (iface=${LAB_IFACE})"
echo "模型: hetero-edge-llm:v1"
echo "其他节点调用示例:"
echo "  curl http://${LAB_IP}:${LAB_PORT}/v1/chat/completions \\"
echo "       -H 'Content-Type: application/json' \\"
echo "       -d '{\"model\":\"hetero-edge-llm:v1\",\"messages\":[{\"role\":\"user\",\"content\":\"...\"}]}'"
echo
echo "注意: 当前对端（昇腾盒子）尚未配置 IP/SSH，链路虽 up 但跨设备调用仍不可用；"
echo "      本脚本只负责把服务绑到实验网口，连通性需先完成地址规划。"
echo

exec env OLLAMA_HOST="${LAB_IP}:${LAB_PORT}" ollama serve
