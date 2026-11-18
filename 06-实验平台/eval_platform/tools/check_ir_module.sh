#!/bin/bash
# 热成像模组（TC1-C + DVP→USB 转接板 / UVC / 0bda:5840）就绪自检
#
# 用法：
#   bash eval_platform/tools/check_ir_module.sh            # 默认查 rv1126b
#   bash eval_platform/tools/check_ir_module.sh rk3588     # 查 RK3588
#   HOST=ascend bash eval_platform/tools/check_ir_module.sh
#
# 重要前提（见《TC1-C 产品说明书》V1.7 §5.1）：
#   “模组自身没有 USB 接口” —— TC1-C 是 40pin DVP/SPI 模组，必须经厂商配的
#   DVP→USB 转接板（就是 0bda:5840 这颗 UVC 桥）才会出现在 USB 总线上。
#   所以“插热成像”= 转接板 USB 头插到板上 + 排线两端都插牢 + 模组 3.3V/5V 供电到位。
set -u
HOST="${1:-${HOST:-rv1126b}}"
VID="0bda"; PID="5840"

say() { printf '%s\n' "$*"; }
ok()  { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad() { printf '  \033[31m✗\033[0m %s\n' "$*"; }
warn(){ printf '  \033[33m!\033[0m %s\n' "$*"; }

ip_of() {
  case "$1" in
    rv1126b) echo "192.168.77.22" ;;
    rk3588)  echo "192.168.77.21" ;;
    ascend)  echo "192.168.77.10" ;;
    *)       echo "-" ;;
  esac
}

say "热成像自检 · 目标 $(ip_of "$HOST")（ssh ${HOST}）"
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST" true 2>/dev/null; then
  bad "SSH 不通：先确认 $(ip_of "$HOST") 在线"; exit 2
fi

# ── 1) USB 层：转接板（UVC 桥）是否枚举 ──
USB=$(ssh -o BatchMode=yes "$HOST" "for d in /sys/bus/usb/devices/*; do
  [ \"\$(cat \$d/idVendor 2>/dev/null)\" = '$VID' ] || continue
  [ \"\$(cat \$d/idProduct 2>/dev/null)\" = '$PID' ] || continue
  printf '%s:%s@%s ' \"\$(cat \$d/idVendor)\" \"\$(cat \$d/idProduct)\" \"\$(basename \$d)\"
done" 2>/dev/null)
if [ -n "$USB" ]; then ok "USB 已枚举：$USB"; USB_OK=1; else
  bad "USB 未枚举：总线上没有 $VID:$PID"; USB_OK=0
fi

# ── 2) 同主机其他 USB 设备（判断是不是整条总线没工作） ──
OTHER=$(ssh -o BatchMode=yes "$HOST" 'lsusb 2>/dev/null | grep -v "root hub" | sed "s/^/    /"' 2>/dev/null)
if [ -n "$OTHER" ]; then say "   当前 USB 设备："; printf '%s\n' "$OTHER"; else warn "该主机 USB 总线上没有任何设备"; fi

# ── 3) 有没有插拔痕迹（判断“插了但没识别”还是“根本没插上”） ──
EV=$(ssh -o BatchMode=yes "$HOST" 'dmesg 2>/dev/null | grep -icE "new (high|full|super)-speed USB device"' 2>/dev/null)
say "   dmesg 中 USB 设备识别次数：${EV:-?}（插拔后会 +1；不变说明电气层没有连通）"

# ── 4) UVC quirk（0bda:5840 的 dwMaxPayloadTransferSize 虚高，需 FIX_BANDWIDTH=0x80） ──
Q=$(ssh -o BatchMode=yes "$HOST" 'cat /sys/module/uvcvideo/parameters/quirks 2>/dev/null')
case "${Q:-}" in
  ""|0) warn "uvcvideo quirks=${Q:-读不到}（建议 128，否则 YUYV 会报带宽不足）" ;;
  128)  ok "uvcvideo quirks=128（FIX_BANDWIDTH 已生效）" ;;
  4294967295) ok "uvcvideo quirks=0xFFFFFFFF（全 quirk，已含 FIX_BANDWIDTH）" ;;
  *)    warn "uvcvideo quirks=${Q}（未包含 0x80 时 YUYV 可能起不来）" ;;
esac

# ── 5) UVC 视频节点（枚举成功后才会有） ──
if [ "$USB_OK" = "1" ]; then
  NODES=$(ssh -o BatchMode=yes "$HOST" 'v4l2-ctl --list-devices 2>/dev/null | grep -A2 -iE "0bda|therm|ir|TC1" | grep "/dev/video" | tr -d "\t "' 2>/dev/null)
  if [ -n "$NODES" ]; then
    ok "UVC 节点：$(printf '%s' "$NODES" | tr '\n' ' ')"
    for v in $NODES; do
      F=$(ssh -o BatchMode=yes "$HOST" "v4l2-ctl -d $v --list-formats 2>/dev/null | grep -E \"\\[[0-9]\\]\" | tr -d '\t'" 2>/dev/null)
      [ -n "$F" ] && say "     $v 格式：$(printf '%s' "$F" | tr '\n' ' ')"
    done
  else
    warn "USB 已枚举但没拿到 /dev/video（多为带宽或驱动绑定失败，重插或重启 uvcvideo 后复测）"
  fi
fi

# ── 6) 业务层：仅 RV1126B 有 camera_stream 的 status.json ──
if [ "$HOST" = "rv1126b" ]; then
  SJ=$(ssh -o BatchMode=yes "$HOST" 'curl -s --max-time 4 http://127.0.0.1:8082/status.json' 2>/dev/null)
  if [ -z "$SJ" ]; then
    bad "8082 status.json 无响应（systemctl status camera-preview）"
  else
    printf '%s' "$SJ" | python3 -c '
import json,sys
d=json.load(sys.stdin)
g=lambda s: print("  \033[32m✓\033[0m "+s)
r=lambda s: print("  \033[31m✗\033[0m "+s)
print(("  \033[32m✓\033[0m " if d.get("ir_ready") else "  \033[31m✗\033[0m ") +
      "ir_ready=%s  ir_state=%s  ir_usb=%s  sdk_running=%s" % (
      d.get("ir_ready"), d.get("ir_state"), d.get("ir_usb"), d.get("sdk_running")))
if d.get("ir_ready"):
    g("热成像 %s fps，%s 帧" % (d.get("ir_fps"), d.get("ir_frame")))
if d.get("ir_error"):
    print("  \033[33m!\033[0m 原因：" + str(d.get("ir_error")))
print("  · 可见光 %s fps（对照，用于确认板子/服务本身正常）" % d.get("vis_fps"))
'
  fi
fi

# ── 7) 结论 ──
say ""
if [ "$USB_OK" = "1" ]; then
  say "结论：转接板已被识别。若画面仍不出，问题在取流/编码层（看上面的格式与 ir_error）。"
else
  say "结论：USB 总线上没有 0bda:5840，属物理连接问题，软件补不了。按顺序查："
  say "  1. 转接板的 USB 头 —— 插在板子的 Type-A 口（LubanCat-5IO 有 1×USB3.0 + 2×USB2.0，"
  say "     不要插 Type-C 那个口，那是烧录用 OTG，默认 device 模式，插什么都不会枚举）"
  say "  2. 排线两端 —— 模组 40pin 端 + 转接板端都要完全压入，金手指方向别反"
  say "  3. 换线/换口 —— 有些线只有供电线芯；换一个 Type-A 口再试"
  say "  4. 插拔后立刻复跑本脚本：dmesg 计数不变 = 电气层完全没连通（线/座/转接板问题）"
fi
