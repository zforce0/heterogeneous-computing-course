#!/bin/bash
# 在 RV1126B 上编译端侧两个检测器（源码即交付物，板端就地编译）：
#   eval_platform/tools/yolo_live.cpp  -> /root/eval_dashboard/yolo_live   （可见光 YOLOv8）
#   eval_platform/tools/ir_detect.cpp  -> /root/eval_dashboard/ir_detect   （热成像 NPU + CV 热斑）
#
# 为什么在板端编译：板载 OpenCV 4.5 与 rknnrt 的运行库就是它要链接的那一份，
# 交叉工具链容易版本错配；板上有 g++，4 核编译各自约 45 s。
# 依赖（板载已有）：/usr/include/opencv4、/usr/lib/librknnrt.so、/userdata/rv1126_test/{include,lib}
#
# 注意：两个二进制都会占 NPU，单帧约 75~110 ms，同时跑时热成像网络推理降到每 2 帧一次。
#
# 用法: bash eval_platform/tools/build_edge_detectors.sh [user@host]
set -euo pipefail

HOST="${1:-rv1126b}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$ROOT/eval_platform/tools"
CXX_FLAGS="-O2 -std=c++11 -I/usr/include/opencv4 -I/userdata/rv1126_test/include"
LIBS="/userdata/rv1126_test/lib/yolov8.cpp /userdata/rv1126_test/lib/yolov8_postprocess.cpp /userdata/rv1126_test/lib/file_utils.cpp \
-lrknnrt -L/usr/lib/aarch64-linux-gnu -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_videoio -ljpeg -lpthread"

echo "[1/4] 同步源码"
ssh "$HOST" "mkdir -p /root/eval_dashboard/src_ir"
scp -q "$TOOLS/yolo_live.cpp" "$HOST:/root/eval_dashboard/yolo_live.cpp"
scp -q "$TOOLS/ir_detect.cpp" "$HOST:/root/eval_dashboard/src_ir/main.cpp"

echo "[2/4] 编译 yolo_live（可见光）"
ssh "$HOST" "cd /root/eval_dashboard && g++ $CXX_FLAGS yolo_live.cpp $LIBS -o yolo_live.new && systemctl stop yolo-live && mv yolo_live.new yolo_live && systemctl start yolo-live"

echo "[3/4] 编译 ir_detect（热成像）"
ssh "$HOST" "cd /root/eval_dashboard/src_ir && g++ $CXX_FLAGS main.cpp $LIBS -o /root/eval_dashboard/ir_detect"

echo "[4/4] 安装/重启 ir-detect 单元"
ssh "$HOST" 'cat > /etc/systemd/system/ir-detect.service <<"EOF"
[Unit]
Description=IR thermal detector (YOLOv8 RKNN + hot-blob) reading /tmp/ir_sdk.raw
After=camera-preview.service
Wants=camera-preview.service

[Service]
Type=simple
ExecStart=/root/eval_dashboard/ir_detect --yolo-every 2
Restart=always
RestartSec=5
Nice=-5

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload && systemctl enable ir-detect.service && systemctl restart ir-detect.service
sleep 5
systemctl is-active yolo-live ir-detect camera-preview
python3 -c "
import json
a=json.load(open(\"/tmp/live_boxes.json\")); b=json.load(open(\"/tmp/ir_boxes.json\"))
print(\"vis model:\", a.get(\"model\"), a.get(\"infer_ms\"), \"ms\")
print(\"ir  model:\", b.get(\"model\"), b.get(\"infer_ms\"), \"ms\")
"'
