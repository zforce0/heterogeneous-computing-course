#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

ONNXRUNTIME_DIR="${ONNXRUNTIME_DIR:-/tmp/onnxruntime-linux-aarch64-1.21.0}"

echo "=== Building detector_test ==="
echo "Project: $PROJECT_DIR"
echo "ONNX Runtime: $ONNXRUNTIME_DIR"

cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DONNXRUNTIME_DIR="$ONNXRUNTIME_DIR"

cmake --build "$BUILD_DIR" -j$(nproc)

echo ""
echo "=== Build complete ==="
echo "Binary: $BUILD_DIR/detector_test"