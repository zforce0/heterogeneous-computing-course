#!/usr/bin/env python3
"""给 RV1126B 的 camera_stream.py 打热成像自愈补丁（幂等）。

修两个真实缺陷：
  1) ir_grabber 只在进程启动时拉起一次 SDK；ir_snap 因模组缺席退出后永不重拉，
     导致"插上模组也得重启服务"。
  2) vis_grabber 每取到一帧可见光就把 last_error 清空，热成像的报错被瞬间覆盖，
     现场看不到真实原因。

另外把"模组未枚举 / SDK 取帧失败"区分开，status.json 增加 ir_usb / ir_state / ir_error。
"""
import re
import shutil
import sys
import time

PATH = "/root/eval_dashboard/camera_stream.py"

NEW_HELPERS = '''
# ── 热成像模组（TC1-C 非制冷红外，UVC；VID:PID 0bda:5840）───────────────
IR_VID, IR_PID = "0bda", "5840"


def ir_usb_present():
    """模组是否已在 USB 总线上枚举（只看 USB 层，与驱动/SDK 无关）。"""
    base = "/sys/bus/usb/devices"
    try:
        names = os.listdir(base)
    except OSError:
        return False
    for n in names:
        p = os.path.join(base, n)
        try:
            with open(os.path.join(p, "idVendor")) as f:
                vid = f.read().strip()
            with open(os.path.join(p, "idProduct")) as f:
                pid = f.read().strip()
        except OSError:
            continue
        if vid == IR_VID and pid == IR_PID:
            return True
    return False

'''

NEW_IR = '''def ir_grabber():
    """取 SDK 输出帧。模组缺席 / 进程退出都能自愈：插上即恢复，无需重启服务。"""
    backoff = 1.0
    while True:
        if ctrl["state"] != "running":
            time.sleep(0.3)
            continue
        try:
            if sdk_proc is None or sdk_proc.poll() is not None:
                ensure_sdk()                      # 守活：SDK 进程没了就重拉
            d = open(IR_RAW, "rb").read()
            w, h, s = struct.unpack("<III", d[:12])
            if w != W or h != H or len(d) < 12 + s * h:
                raise ValueError("SDK frame not ready")
            payload = rgb_png(w, h, s, d[12:12 + s * h])
            now = time.monotonic()
            with lock:
                ctrl["ir"] = payload
                ctrl["ir_frame"] += 1
                ctrl["ir_bytes"] = len(payload)
                ctrl["_ir_ts"].append(now)
                ctrl["ir_fps"] = fps(ctrl["_ir_ts"], now)
                ctrl["ir_error"] = ""
                ctrl["ir_state"] = "streaming"
            backoff = 1.0
        except Exception as exc:
            usb = ir_usb_present()
            with lock:
                ctrl["ir_state"] = "sdk_error" if usb else "no_device"
                ctrl["ir_error"] = (
                    "模组未枚举（%s:%s 不在 USB 总线）" % (IR_VID, IR_PID) if not usb
                    else "SDK 取帧失败: " + str(exc)[:90])
            if not usb:
                stop_sdk()                        # 别让失败的进程占住设备
                time.sleep(3.0)                   # 3s 一轮盯着 USB 热插拔
            else:
                time.sleep(backoff)
                backoff = min(5.0, backoff * 1.5)
            continue
        time.sleep(0.12)
'''

src = open(PATH, encoding="utf-8").read()
orig = src

# 1) 备份（同一秒内重复执行只留一份）
bak = PATH + ".bak." + time.strftime("%Y%m%d-%H%M%S")
shutil.copy2(PATH, bak)

# 2) 常量 + USB 探测助手（插在 ctrl 定义之前）
if "def ir_usb_present" not in src:
    anchor = 'SDK_LIB = "/userdata/dual_demo/libs/arm64"'
    assert anchor in src, "找不到 SDK_LIB 锚点"
    src = src.replace(anchor, anchor + "\n" + NEW_HELPERS, 1)

# 3) ctrl 增加独立错误/状态字段
if '"ir_error"' not in src:
    old = '"last_error": "", "ir_source": "vendor_sdk",'
    assert old in src, "找不到 ctrl.last_error 锚点"
    src = src.replace(old,
                      '"last_error": "", "vis_error": "", "ir_error": "",\n'
                      '    "ir_state": "init", "ir_source": "vendor_sdk",', 1)

# 4) 可见光不再清空 last_error（改成只清自己的 vis_error）
old_vis = '''                ctrl["vis_fps"] = fps(ctrl["_vis_ts"], now)
                ctrl["last_error"] = ""'''
new_vis = '''                ctrl["vis_fps"] = fps(ctrl["_vis_ts"], now)
                ctrl["vis_error"] = ""
                ctrl["last_error"] = ctrl.get("ir_error") or ""'''
assert old_vis in src, "找不到 vis_grabber 清错锚点"
src = src.replace(old_vis, new_vis, 1)

old_vis_err = '''            with lock:
                ctrl["last_error"] = "vis: " + str(exc)[:110]'''
new_vis_err = '''            with lock:
                ctrl["vis_error"] = "vis: " + str(exc)[:110]
                ctrl["last_error"] = ctrl.get("ir_error") or ctrl["vis_error"]'''
assert old_vis_err in src, "找不到 vis_grabber 报错锚点"
src = src.replace(old_vis_err, new_vis_err, 1)

# 5) 替换整个 ir_grabber
m = re.search(r"def ir_grabber\(\):.*?\n(?=\n\nclass Handler)", src, re.S)
assert m, "找不到 ir_grabber 函数块"
src = src[:m.start()] + NEW_IR + src[m.end():]

# 6) status.json 增补字段
old_snap = '''            snap["sdk_running"] = sdk_proc is not None and sdk_proc.poll() is None'''
new_snap = '''            snap["sdk_running"] = sdk_proc is not None and sdk_proc.poll() is None
            snap["ir_usb"] = ir_usb_present()
            snap["ir_vid_pid"] = "%s:%s" % (IR_VID, IR_PID)'''
assert old_snap in src, "找不到 status 锚点"
src = src.replace(old_snap, new_snap, 1)

if src == orig:
    print("no change needed")
    sys.exit(0)

open(PATH, "w", encoding="utf-8").write(src)
print("patched:", PATH)
print("backup :", bak)
