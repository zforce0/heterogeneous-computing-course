#!/bin/bash
# build.sh - 红外测温功能验证程序构建脚本
#
# 用法:
#   ./build.sh          # 默认 native 编译 (aarch64 Ubuntu)
#   ./build.sh clean    # 清理 build/
#
# 默认 SDK 路径: aarch64 native (linux_ac010):
#   /mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs
# 找不到时回退到主项目 twocameraOnMAC/libs/native

set -e

cd "$(dirname "$0")"

MODE="${1:-native}"

if [ "$MODE" = "clean" ]; then
    rm -rf build/
    echo "[build] cleaned"
    exit 0
fi

mkdir -p build
cd build

DEFAULT_SDK="/mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs"

if [ ! -d "$DEFAULT_SDK" ]; then
    echo "[build] aarch64 SDK not found: $DEFAULT_SDK"
    echo "       fallback to twocameraOnMAC/libs/native"
    SDK_DIR="$(cd .. && pwd)/../../libs/native"
else
    SDK_DIR="$DEFAULT_SDK"
fi

echo "[build] native compile (g++)  SDK=$SDK_DIR"
cmake .. \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_BUILD_TYPE=Debug \
    -DIR_TEMP_SDK_LIB_DIR="$SDK_DIR"

# hgfs 共享盘上并行编译偶发 .o 截断, 强制单线程构建
cmake --build . --parallel 1 2>&1 | tail -40

echo "[build] output: $(pwd)/ir_temperature_test"
file ir_temperature_test 2>/dev/null || true
