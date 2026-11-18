#!/bin/bash
# 一键启动脚本(在板端 rv1126_test 目录下运行)
# 用法: ./run.sh [任意程序参数]
#   ./run.sh                          # 默认启动:IR 256x192@25 + VIS 1280x720@25
#   ./run.sh -W 640 -H 480            # 降低可见光分辨率
#   ./run.sh -d /dev/videoN           # 手动指定可见光节点
#   ./run.sh --help                   # 查看程序全部参数
# Ctrl+C 退出并打印两路帧率与推理/存图统计
#
# 相机掉线自动恢复(红外/可见光分别检测):
#   - 程序因相机打开失败退出(退出码 3)时自动重试(RETRIES 次,间隔 INTERVAL 秒)
#   - 设备还挂在总线上:对该设备做 authorized 0->1 重新枚举
#   - 设备已从总线消失:对 USB Hub 重新枚举(注意:会短暂断开同 Hub 上的网卡,
#     SSH 连接可能闪断,HUB_RESET=0 可关闭此行为,改为提示拔插相机)
trap '' HUP   # SSH 闪断时不被挂断信号杀死
cd "$(dirname "$0")"

BIN=build/rv1126_dual_demo
MODEL=yolov8m_rv1126b.rknn

# 两路相机 USB VID:PID
VIS_VID=1d6c; VIS_PID=0103   # 可见光
IR_VID=0bda;  IR_PID=5840    # 红外
HUB_VID=05e3;  HUB_PID=0610  # 外置 USB Hub
RETRIES=${RETRIES:-15}       # 相机失败重试次数
INTERVAL=${INTERVAL:-3}      # 重试间隔(秒)
HUB_RESET=${HUB_RESET:-1}    # 设备消失时是否重置 Hub(1=是)

# 二进制不存在时自动编译
if [ ! -x "$BIN" ]; then
    echo "[run] binary not found, building..."
    bash build.sh
fi

# 模型文件不存在时给出提示(可用 ./deploy.sh 从开发机同步)
if [ ! -f "$MODEL" ]; then
    echo "[run] ERROR: model $MODEL not found"
    exit 1
fi

# 按 VID:PID 查找 sysfs 中的 USB 设备路径(找不到返回非 0)
find_usb_dev() {
    local want_vid=$1 want_pid=$2 d
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        if [ "$(cat "$d/idVendor" 2>/dev/null)" = "$want_vid" ] && \
           [ "$(cat "$d/idProduct" 2>/dev/null)" = "$want_pid" ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

# 单个设备的恢复:优先单独重枚举;设备消失时最后手段重置 Hub
recover_one() {
    local want_vid=$1 want_pid=$2 name=$3 dev hub
    if dev=$(find_usb_dev "$want_vid" "$want_pid"); then
        echo "[run] recover($name): re-enumerate $dev"
        echo 0 > "$dev/authorized" 2>/dev/null
        sleep 1
        echo 1 > "$dev/authorized" 2>/dev/null
        return
    fi
    if [ "$HUB_RESET" = "1" ] && hub=$(find_usb_dev "$HUB_VID" "$HUB_PID"); then
        echo "[run] recover($name): device absent, reset USB hub ($hub)"
        echo "[run] NOTE: devices on this hub will re-enumerate, ssh may blip"
        echo 0 > "$hub/authorized" 2>/dev/null
        sleep 3
        echo 1 > "$hub/authorized" 2>/dev/null
    else
        echo "[run] recover($name): device absent, please unplug/replug its USB"
    fi
}

attempt=0
while true; do
    echo "[run] starting: $BIN $MODEL $*"
    "$BIN" "$MODEL" "$@"
    rc=$?
    # rc=0: Ctrl+C 正常退出;rc=3: 相机打开失败(重试);其他: 不重试
    if [ $rc -eq 0 ] || [ $rc -ne 3 ]; then
        exit $rc
    fi
    attempt=$((attempt + 1))
    if [ $attempt -ge "$RETRIES" ]; then
        echo "[run] camera still failing after $RETRIES retries, giving up"
        exit 3
    fi
    echo "[run] camera open failed (rc=3), retry $attempt/$RETRIES in ${INTERVAL}s..."
    # 程序已退出,SDK 自然释放 USB claim;对两路设备分别检查恢复
    recover_one "$IR_VID"  "$IR_PID"  "ir"
    recover_one "$VIS_VID" "$VIS_PID" "vis"
    sleep "$INTERVAL"
done
