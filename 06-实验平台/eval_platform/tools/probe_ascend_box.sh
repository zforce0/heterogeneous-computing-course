#!/usr/bin/env bash
# 在昇腾盒子【本机】执行，采集设备详细信息。
# 用法（从 Mac）： ssh ascend 'bash -s' < eval_platform/tools/probe_ascend_box.sh | tee ascend_probe.txt
# 前置：~/.ssh/config 已配好 ascend 别名且免密可用。
# 设计原则：命令不存在就标 N/A，绝不猜值、绝不填 0；所有外部命令带 timeout，避免挂起。
#
# 2026-11-14 修订（首轮实跑暴露的问题）：
#   1) `npu-smi info -t <type>` 必须带 -i 0 -c 0，否则只打印 usage 不返回数据；
#   2) `atc --version` / `msame` 可能无输出且不退出，必须加 timeout；
#   3) version.info 文件很多，早期脚本在第 2 节就被截断，改为只读关键字段；
#   4) 自动判定表按首轮实测（310B1 / 20TOPS / 24GB）重写，不再假设 8TOPS/4GB。
set -u

T=8
sec()  { printf '\n===== %s =====\n' "$1"; }
try()  { local c="$1"; shift; if command -v "$c" >/dev/null 2>&1; then timeout "$T" "$c" "$@" 2>&1 || echo "(timeout/err)"; else echo "N/A (command not found: $c)"; fi; }
show() { if [ -r "$1" ]; then head -c 1200 "$1"; else echo "N/A (not readable: $1)"; fi; }

sec "0. 采集元信息"
echo "captured_at_board : $(date '+%Y-%m-%d %H:%M:%S %z' 2>/dev/null || echo N/A)"
echo "hostname          : $(hostname 2>/dev/null || echo N/A)"
echo "NOTE              : 板端 RTC 曾实测漂移（本盒显示 2025-08，实际 2026-11），时间不可用于跨设备对账"

sec "1. NPU / 昇腾芯片（最关键）"
try npu-smi info
for t in board memory product usages; do
  echo "--- npu-smi info -t $t -i 0 -c 0 ---"
  try npu-smi info -t "$t" -i 0 -c 0
done
echo "--- /dev/davinci* ---"; ls -l /dev/davinci* 2>/dev/null || echo "N/A"
echo "--- lspci (Huawei/Ascend) ---"
if command -v lspci >/dev/null 2>&1; then timeout "$T" lspci 2>/dev/null | grep -iE 'huawei|ascend|npu|acceler|310|910' || echo "(no match)"; else echo "N/A"; fi

sec "2. CANN / 驱动 / LLM 运行时"
echo "--- toolkit version.cfg（关键字段） ---"
if [ -r /usr/local/Ascend/ascend-toolkit/latest/version.cfg ]; then
  grep -m1 -E 'toolkit_running_version|Version=' /usr/local/Ascend/ascend-toolkit/latest/version.cfg || echo N/A
  grep -m1 'version_dir' /usr/local/Ascend/ascend-toolkit/latest/acllib/version.info 2>/dev/null || true
else echo "N/A"; fi
echo "--- 工具链目录 ---"; ls -d /usr/local/Ascend/* 2>/dev/null | head -12
echo "--- ATC ---"; try atc --version
echo "--- LLM 运行时探测 ---"
for c in mindie mindieservice llama-cli llama-server ollama; do
  printf '%-16s ' "$c"; command -v "$c" 2>/dev/null || echo absent
done
python3 -c "import importlib.util as u; print('python pkgs:', {m: bool(u.find_spec(m)) for m in ['torch','torch_npu','transformers','acl','mindie_llm']})" 2>&1 | head -2
echo "--- 环境变量 ---"; env | grep -iE 'ASCEND|CANN' | head -6 || echo "N/A"

sec "3. CPU / 内存 / 存储 / OS"
echo "--- uname -a ---";       uname -a 2>/dev/null || echo N/A
echo "--- OS ---";             grep -m1 PRETTY_NAME /etc/os-release 2>/dev/null || echo N/A
echo "--- CPU cores ---";      nproc 2>/dev/null || echo N/A
echo "--- MemTotal ---";       grep -m1 MemTotal /proc/meminfo 2>/dev/null || echo N/A
echo "--- free -h ---";        try free -h
echo "--- df -h / ---";        try df -h /

sec "4. 网络 / 服务 / 外网连通性"
echo "--- ip addr ---";        try ip -brief addr
echo "--- ip route ---";       try ip route
echo "--- 监听端口 ---";        (ss -ltnp 2>/dev/null || netstat -ltnp 2>/dev/null) | head -14 || echo N/A
echo "--- 外网连通（决定能否在线装包） ---"
timeout 6 curl -s -o /dev/null -w 'http_code=%{http_code}\n' https://pypi.org/simple/ 2>/dev/null || echo "无外网 / 超时"

sec "5. 自动判定：LLM 最强节点"
CHIP="$(npu-smi info -t board -i 0 -c 0 2>/dev/null | awk -F: '/Chip Name/{gsub(/ /,"",$2); print $2}')"
PROD="$(npu-smi info -t product -i 0 -c 0 2>/dev/null | awk -F: '/Product Type/{gsub(/^ +/,"",$2); print $2}')"
CAP="$(npu-smi info -t memory -i 0 -c 0 2>/dev/null | awk -F: '/Capacity/{gsub(/ /,"",$2); print $2}')"
MEM_GB="$(awk '/MemTotal/{printf "%.1f", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo N/A)"
echo "chip_name       : ${CHIP:-N/A}"
echo "product_type    : ${PROD:-N/A}"
echo "npu_mem_MB      : ${CAP:-N/A}"
echo "system_mem_GB   : ${MEM_GB}"
HAS_LLM=no
for c in mindie mindieservice llama-server ollama; do command -v "$c" >/dev/null 2>&1 && HAS_LLM=yes; done
echo "llm_runtime     : ${HAS_LLM}"
echo
if [ "${CHIP:-}" = "310B1" ] || [ "${PROD:-}" = "Atlas 200I A2" ]; then
  echo "verdict_hardware : 昇腾盒子 = 20 TOPS INT8 / 10 TFLOPS FP16 / 24GB —— 原始 AI 算力与容量全场第一"
  echo "verdict_practical: 但 51.2GB/s 带宽 < M1 的 68.25GB/s，且需确认/安装 LLM 运行时"
  if [ "$HAS_LLM" = "no" ]; then
    echo "verdict_current  : 当前唯一可跑且实测的 LLM 节点仍是 MacBook Air M1（hetero-edge-llm:v1, 9.34 tok/s）"
    echo "next             : 若要让昇腾名副其实，需离线装 MindIE / llama.cpp-CANN + 转模型 + 实测 tok/s"
  else
    echo "verdict_current  : 已检测到 LLM 运行时，需实跑 benchmark 与 M1 对比后再定案"
  fi
else
  echo "verdict          : 芯片 ${CHIP:-N/A} 不在判定表，请补充规格后重算（禁止猜测）"
fi
echo
echo "提示：把本输出粘贴回来，据此更新 heterogeneous_setup.yaml 与 V1.2 设置文档的算力判定。"
