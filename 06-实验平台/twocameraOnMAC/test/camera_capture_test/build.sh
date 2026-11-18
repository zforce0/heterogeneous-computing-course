#!/bin/bash
# build.sh - 双摄标定拍照测试程序构建脚本
#
# 用法:
#   ./build.sh          # 默认 native 编译 (aarch64 Ubuntu)
#   IR_SDK_LIB_DIR=... ./build.sh
#   ./build.sh clean

set -e

cd "$(dirname "$0")"

MODE="${1:-build}"

if [ "$MODE" = "clean" ]; then
    rm -rf build/
    echo "[build] cleaned"
    exit 0
fi

DEFAULT_SDK="/mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs"
SDK_DIR="${IR_SDK_LIB_DIR:-$DEFAULT_SDK}"

echo "[build] native compile (g++)  SDK=$SDK_DIR"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIR_SDK_LIB_DIR="$SDK_DIR"

# hgfs 共享盘上并行编译偶发 .o 截断, 强制单线程构建
cmake --build build --parallel 1 2>&1 | tail -10

echo "[build] output: $(pwd)/build/camera_capture_test"
echo "[run]   cd 项目根目录后运行: test/camera_capture_test/build/camera_capture_test"
