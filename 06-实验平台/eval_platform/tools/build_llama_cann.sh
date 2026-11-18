#!/bin/bash
# 在昇腾盒子 (Atlas 200I A2 / 310B1 / CANN 8.0.RC3 / Ubuntu 22.04) 上编译 llama.cpp
# 目标：产出同时含 CPU(ARM NEON) 与 CANN(NPU) 双后端的 llama-server。
# 用法（在 Mac 上）：ssh ascend 'bash -s' < eval_platform/tools/build_llama_cann.sh
set -o pipefail
BASE=/opt/g4-llm
LOG=$BASE/logs
mkdir -p "$LOG"
cd "$BASE/src" || exit 1

echo "=== [1/3] 加载 CANN 环境 ==="
source /usr/local/Ascend/ascend-toolkit/set_env.sh
echo "ASCEND_TOOLKIT_HOME=$ASCEND_TOOLKIT_HOME"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" | cut -c1-200
npu-smi info -t board -i 0 -c 0 2>/dev/null | grep -E 'Chip Name|Chip Version|Board ID' || true

echo "=== [2/3] cmake configure (GGML_CANN=ON) ==="
rm -rf build-cann
cmake -B build-cann \
    -DGGML_CANN=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DSOC_TYPE=Ascend310B1 \
    -DUSE_ACL_GRAPH=OFF \
    -DLLAMA_CURL=OFF \
    . 2>&1 | tee "$LOG/cmake_cann.log"
CFG=${PIPESTATUS[0]}
if [ "$CFG" -ne 0 ]; then
    echo "!!! CANN configure 失败，回退纯 CPU 构建 ==="
    rm -rf build-cpu
    cmake -B build-cpu -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF . 2>&1 | tee "$LOG/cmake_cpu.log" || exit 2
    cmake --build build-cpu -j4 --target llama-server 2>&1 | tee "$LOG/build_cpu.log" || exit 3
    echo "BUILD_RESULT=CPU_FALLBACK"
    exit 0
fi

echo "=== [3/3] 编译 llama-server (-j4) ==="
cmake --build build-cann -j4 --target llama-server 2>&1 | tee "$LOG/build_cann.log"
RC=${PIPESTATUS[0]}
if [ "$RC" -ne 0 ]; then
    echo "!!! CANN 编译失败，回退纯 CPU 构建 ==="
    rm -rf build-cpu
    cmake -B build-cpu -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF . 2>&1 | tee "$LOG/cmake_cpu.log" || exit 4
    cmake --build build-cpu -j4 --target llama-server 2>&1 | tee "$LOG/build_cpu.log" || exit 5
    echo "BUILD_RESULT=CPU_FALLBACK"
    exit 0
fi

echo "=== 产物 ==="
find build-cann -name 'llama-server' -type f -exec ls -lh {} \;
echo "BUILD_RESULT=CANN_OK"
