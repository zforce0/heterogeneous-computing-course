#!/bin/bash
# 构建脚本:
# - 在 aarch64(板端)直接本机编译
# - 在 x86 构建机上用 ~/configs/cross.cmake 工具链交叉编译(需先搭建 sysroot,见 实现思路.md)
set -e
cd "$(dirname "$0")"

ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ]; then
    echo "[build] native build on board ($ARCH)"
    cmake -B build -DCMAKE_BUILD_TYPE=Release .
    cmake --build build -j"$(nproc)"
else
    TOOLCHAIN="$HOME/configs/cross.cmake"
    if [ -f "$TOOLCHAIN" ]; then
        echo "[build] cross build via $TOOLCHAIN"
        cmake -B build -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release .
        cmake --build build -j"$(nproc)"
    else
        echo "[build] ERROR: not on aarch64 and no cross toolchain file at $TOOLCHAIN"
        echo "[build]        deploy sources to the board and build there, see deploy.sh"
        exit 1
    fi
fi

echo "[build] done: $(pwd)/build/rv1126_yolov8_demo"
