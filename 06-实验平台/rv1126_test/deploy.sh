#!/bin/bash
# 部署脚本:同步源码与模型到 RV1126B 板端并在板上编译
# 用法: ./deploy.sh [板端IP或 root@IP,默认 root@192.168.3.248]
# 说明: ssh 首次连接需输入密码(板端 root 密码),建议 ssh-copy-id 配置免密
set -e
cd "$(dirname "$0")"

BOARD=${1:-root@192.168.3.252}
REMOTE_DIR=/userdata/rv1126_test
MODEL_SRC=/mnt/hgfs/share/code/yolov8_detect_C_demo/yolov8_detect_demo_release/yolov8m_rv1126b.rknn

echo "[deploy] sync sources -> $BOARD:$REMOTE_DIR"
ssh "$BOARD" "mkdir -p $REMOTE_DIR"
tar czf - --exclude=build --exclude=output --exclude='*.md' . | ssh "$BOARD" "tar xzf - -C $REMOTE_DIR"

if [ -f "$MODEL_SRC" ]; then
    echo "[deploy] sync model"
    scp -q "$MODEL_SRC" "$BOARD:$REMOTE_DIR/"
fi

echo "[deploy] build on board"
ssh "$BOARD" "cd $REMOTE_DIR && bash build.sh"

echo "[deploy] done. run on board:"
echo "  ssh $BOARD"
echo "  cd $REMOTE_DIR && ./run.sh"
