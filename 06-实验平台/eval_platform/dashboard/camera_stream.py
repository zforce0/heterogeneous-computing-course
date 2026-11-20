#!/usr/bin/env python3
"""Controllable real-time camera preview for the benchmark dashboard (RV1126B).

Endpoints on 192.168.77.22:8082:
  /vis.jpg            latest visible-light JPEG (V4L2 MJPG, 640x480)
  /ir.png             latest thermal grayscale PNG (vendor SDK, 256x192, native orientation)
  /status.json        state, live fps, cumulative frames, source, errors
                      ir_det: 端侧热成像检测结果（YOLOv8 RKNN 框 + 传统 CV 热斑）
  /control?action=start|pause|stop
                      start  -> resume capture and spawn SDK adapter
                      pause  -> freeze capture, keep last frame
                      stop   -> terminate SDK adapter and release V4L2 devices

V4L2 devices are exclusive. Use stop before running capture experiments.
Thermal view is grayscale relative radiance; no pseudo-color, no calibrated Celsius.
"""
import collections
import hashlib
import http.server
import json
import os
import socketserver
import struct
import subprocess
import threading
import time
import zlib

# 可见光由 yolo_live 独占 V4L2 设备并写 /tmp/live_frame.jpg，本进程不再直接开设备。
# 设备编号会随 USB 插口变化，g4-guard 维护稳定别名 /dev/g4-vis（见 /tmp/g4-cam.json）。
IR_SNAP = "/root/eval_dashboard/ir_snap"
IR_RAW = "/tmp/ir_sdk.raw"
SDK_LIB = "/userdata/dual_demo/libs/arm64"

# ── 热成像模组（TC1-C 非制冷红外，UVC；VID:PID 0bda:5840）───────────────
IR_VID, IR_PID = "0bda", "5840"
# 连续这么多帧全 0 就认为 SDK 没真取到数据（约 1.5 s，正常画面有噪声，不会全 0）
IR_BLACK_LIMIT = 12
# SDK 长时间运行后会「循环写同一帧」：mtime 照常增长、内容却一字不变，
# 页面显示 5fps/streaming 但画面静止。连续这么多帧内容完全相同即判冻住并重建 SDK。
IR_DUP_LIMIT = 20
# yolo_live 的产物文件超过这么久没更新，就认为可见光断流（按时间算，不按轮数算）
VIS_STALE_SEC = 1.6
# 端侧热成像检测器（ir_detect）的产物：IR 只有 ~5 fps，2.5 s 没更新就当作它没在跑
IR_DET_JSON = "/tmp/ir_boxes.json"
IR_DET_STALE = 2.5


def ir_usb_probe():
    """模组在 USB 总线上的实际位置（只看 USB 层，与驱动/SDK 无关）。

    厂商 SDK（ir_snap）以 usbfs 方式 claim 接口，uvcvideo 就绑不上，也就不会有
    /dev/video* 节点。所以"线插没插、插在哪个口"必须以 USB 设备节点为准；
    用 video4linux 判断会把正常出图的模组误报成"未枚举"。
    """
    base = "/sys/bus/usb/devices"
    info = {"present": False, "port": None, "speed": None, "busnum": None,
            "devnum": None, "product": None, "controller": None, "mode": None}
    try:
        names = os.listdir(base)
    except OSError:
        return info
    for n in names:
        if ":" in n:                                # 跳过 4-1.2:1.0 这类接口节点
            continue
        p = os.path.join(base, n)
        try:
            vid = open(os.path.join(p, "idVendor")).read().strip().lower()
            pid = open(os.path.join(p, "idProduct")).read().strip().lower()
        except OSError:
            continue
        if vid != IR_VID or pid != IR_PID:
            continue

        def rd(attr, _p=p):
            try:
                return open(os.path.join(_p, attr)).read().strip()
            except OSError:
                return None

        drivers = set()
        try:
            for c in os.listdir(p):
                if ":" in c:
                    link = os.path.join(p, c, "driver")
                    if os.path.exists(link):
                        drivers.add(os.path.basename(os.path.realpath(link)))
        except OSError:
            pass
        busnum = rd("busnum")
        info.update({
            "present": True,
            "port": n,                              # sysfs 目录名 = busnum-devpath
            "speed": rd("speed"),
            "busnum": busnum,
            "devnum": rd("devnum"),
            "product": rd("product"),
            "controller": ("usb" + busnum) if busnum else None,
            "mode": ("usbfs" if "usbfs" in drivers
                     else "v4l2" if "uvcvideo" in drivers else "usb_only"),
        })
        return info
    return info


def ir_usb_present():
    """模组是否已在 USB 总线上枚举（只看 USB 层，与驱动/SDK 无关）。"""
    return ir_usb_probe()["present"]


# 绑 0.0.0.0：原先写死 192.168.77.22，网口还没拿到地址时 bind 会失败，
# 导致 systemd 无限重启（历史上累计 6 万余次）。绑通配地址与网口状态解耦。
HOST = os.environ.get("G4_BIND", "0.0.0.0")
PORT = int(os.environ.get("G4_PORT", "8082"))
W, H = 256, 192

lock = threading.Lock()
sdk_proc = None
ctrl = {
    "state": "running",
    "vis_frame": 0, "ir_frame": 0,
    "vis_fps": 0.0, "ir_fps": 0.0,
    "vis_bytes": 0, "ir_bytes": 0,
    "vis": b"", "ir": b"",
    "started_at": time.time(), "last_change": "service start",
    "last_error": "", "vis_error": "", "ir_error": "",
    "ir_state": "init", "ir_source": "vendor_sdk",
    "vis_state": "init",
    "thermal_mode": "relative", "ir_mode": "grayscale", "ir_rotation": "none",
    "_vis_ts": collections.deque(), "_ir_ts": collections.deque(),
}


def fps(deq, now):
    while deq and now - deq[0] > 2.0:
        deq.popleft()
    return round(len(deq) / 2.0, 2) if deq else 0.0


def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)


def rgb_png(w, h, stride, bgr):
    """BGR → RGB 编码为 PNG，保持模组原始朝向（256×192 横向），不做旋转。"""
    raw = bytearray()
    for r in range(h):
        row = bgr[r * stride:r * stride + w * 3]
        if len(row) < w * 3:
            raise ValueError("short BGR row")
        px = bytearray(row)
        px[0::3] = row[2::3]
        px[2::3] = row[0::3]
        raw += b"\x00" + bytes(px)
    return (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
            png_chunk(b"IEND", b""))


def uvc_bound_ifaces():
    """热成像模组上被 uvcvideo 绑着的接口名（如 4-1.2:1.0）。"""
    base = "/sys/bus/usb/devices"
    out = []
    try:
        names = os.listdir(base)
    except OSError:
        return out
    for n in names:
        if ":" in n:                                # 跳过接口节点，只看设备
            continue
        p = os.path.join(base, n)
        try:
            vid = open(os.path.join(p, "idVendor")).read().strip().lower()
            pid = open(os.path.join(p, "idProduct")).read().strip().lower()
        except OSError:
            continue
        if vid != IR_VID or pid != IR_PID:
            continue
        try:
            for c in os.listdir(p):
                if not c.startswith(n + ":"):
                    continue
                drv = os.path.join(base, c, "driver")
                if (os.path.exists(drv)
                        and os.path.basename(os.path.realpath(drv)) == "uvcvideo"):
                    out.append(c)
        except OSError:
            pass
    return out


def release_uvc():
    """把 uvcvideo 从热成像接口上摘下来，让厂商 SDK 能用 usbfs 独占。

    SDK 启动时若设备已被 uvcvideo 抢绑，libusb 拿不到接口，它就只写一份
    "格式合法、内容全 0"的帧——看着在出流，其实全黑。这是最容易误判的一种坏状态。
    """
    freed = []
    for iface in uvc_bound_ifaces():
        try:
            with open("/sys/bus/usb/drivers/uvcvideo/unbind", "w") as f:
                f.write(iface)
            freed.append(iface)
        except OSError:
            pass
    return freed


def ensure_sdk():
    global sdk_proc
    with lock:
        if sdk_proc is not None and sdk_proc.poll() is None:
            return
    release_uvc()                                   # 必须先摘 uvcvideo，否则 SDK 抢不到设备
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = SDK_LIB
    with lock:
        sdk_proc = subprocess.Popen([IR_SNAP], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_sdk():
    global sdk_proc
    with lock:
        p = sdk_proc
        sdk_proc = None
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=3)
        except Exception:
            p.kill()
    try:
        os.remove(IR_RAW)
    except OSError:
        pass


def vis_grabber():
    # yolo_live 独占 V4L2 设备并写 /tmp/live_frame.jpg，本进程只读它的产物。
    # 但「文件存在」不等于「有在更新」：设备掉线后 yolo_live 会一直等设备而不写新帧，
    # 旧文件留在原地。若不比对更新，就会把最后一帧反复当新帧推出去——
    # 页面看着在收帧（帧号还在涨），画面其实完全不动。
    src = "/tmp/live_frame.jpg"
    last_sig = None
    last_new = time.monotonic()
    stale = False
    while True:
        if ctrl["state"] != "running":
            time.sleep(0.2)
            continue
        try:
            st = os.stat(src)
            sig = (st.st_mtime_ns, st.st_size)
            if sig == last_sig:
                # 轮询要快（端侧 7.5 fps，轮询 0.1s 会漏帧、页面上报的 fps 会偏低），
                # 但「多久没新帧算断流」得按时间算，不能按轮数算。
                if not stale and time.monotonic() - last_new > VIS_STALE_SEC:
                    stale = True
                    with lock:
                        ctrl["vis"] = b""               # 不再把旧帧当就绪帧推出去
                        ctrl["vis_fps"] = 0.0
                        ctrl["vis_state"] = "stale"
                        ctrl["vis_error"] = "可见光没有新帧（yolo_live 未输出，设备可能掉线）"
                time.sleep(0.03)
                continue
            last_sig = sig
            last_new = time.monotonic()
            stale = False
            data = open(src, "rb").read()
            if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
                raise ValueError("yolo_live frame incomplete")
            now = time.monotonic()
            with lock:
                ctrl["vis"] = data
                ctrl["vis_frame"] += 1
                ctrl["vis_bytes"] = len(data)
                ctrl["_vis_ts"].append(now)
                ctrl["vis_fps"] = fps(ctrl["_vis_ts"], now)
                ctrl["vis_state"] = "streaming"
                ctrl["vis_error"] = ""
                ctrl["last_error"] = ctrl.get("ir_error") or ""
        except Exception as exc:
            with lock:
                ctrl["vis_error"] = "vis: " + str(exc)[:110]
                ctrl["vis_state"] = "stale"
                ctrl["last_error"] = ctrl.get("ir_error") or ctrl["vis_error"]
            time.sleep(0.8)
        time.sleep(0.02)


def ir_grabber():
    """取 SDK 输出帧。模组缺席 / 进程退出 / SDK 空转都能自愈，无需重启服务。"""
    backoff = 1.0
    black_run = 0
    dup_run = 0
    last_raw_md5 = None
    last_restart = 0.0
    last_sig = None
    while True:
        if ctrl["state"] != "running":
            time.sleep(0.3)
            continue
        try:
            if sdk_proc is None or sdk_proc.poll() is not None:
                ensure_sdk()                      # 守活：SDK 进程没了就重拉
            st = os.stat(IR_RAW)
            # SDK 实际只出 ~5 fps；以前不比对更新时间，同一帧被反复编码推送，
            # 上报的 ir_fps 会虚高到 6.5（看着更流畅，其实全是重复帧），
            # 还白占一块 CPU 做 zlib。这里只处理真正的新帧。
            sig = (st.st_mtime_ns, st.st_size)
            if sig == last_sig:
                time.sleep(0.03)
                continue
            last_sig = sig
            d = open(IR_RAW, "rb").read()
            w, h, s = struct.unpack("<III", d[:12])
            if w != W or h != H or len(d) < 12 + s * h:
                raise ValueError("SDK frame not ready")
            raw = d[12:12 + s * h]
            if not any(raw):
                # SDK 抢不到设备（被 uvcvideo 绑着）时会循环写全 0：
                # 帧头合法、内容全黑。不查内容就会把"死流"当成正常出流——
                # 页面一片黑，状态却显示 streaming，点重连当然也没用。
                black_run += 1
                if black_run >= IR_BLACK_LIMIT:
                    with lock:
                        ctrl["ir"] = b""           # 不再把死流当就绪帧推给页面
                        ctrl["ir_fps"] = 0.0
                        ctrl["ir_state"] = "sdk_error"
                        ctrl["ir_error"] = ("SDK 未取到有效数据（连续 %d 帧全黑），已重建 SDK"
                                            % black_run)
                    now = time.time()
                    if now - last_restart > 10:
                        last_restart = now
                        stop_sdk()
                        ensure_sdk()               # 内部会先把 uvcvideo 摘掉
                    black_run = 0
                    time.sleep(1.0)
                    continue
                time.sleep(0.12)
                continue
            black_run = 0
            # SDK 冻住检测：mtime 会变，但内容一字不变 → 画面静止却显示 streaming。
            # 用原始帧内容的 md5 判定，连续 IR_DUP_LIMIT 帧相同就重建 SDK。
            cur_md5 = hashlib.md5(raw).digest()
            if cur_md5 == last_raw_md5:
                dup_run += 1
            else:
                dup_run = 0
                last_raw_md5 = cur_md5
            if dup_run >= IR_DUP_LIMIT:
                with lock:
                    ctrl["ir_fps"] = 0.0
                    ctrl["ir_state"] = "sdk_error"
                    ctrl["ir_error"] = ("SDK 画面冻结（连续 %d 帧相同），已重建 SDK" % dup_run)
                now = time.time()
                if now - last_restart > 10:
                    last_restart = now
                    stop_sdk()
                    ensure_sdk()               # 重启取帧进程，让热成像重新出帧
                dup_run = 0
                last_raw_md5 = None
                time.sleep(2.0)                # 给 SDK 恢复时间，别立刻又读旧帧
                continue
            payload = rgb_png(w, h, s, raw)
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
            usb = ir_usb_probe()
            with lock:
                if usb["present"]:
                    # 线插着、设备也在总线上，那就是 SDK 取帧的问题——和线缆无关
                    ctrl["ir_state"] = "sdk_error"
                    ctrl["ir_error"] = "SDK 取帧失败（口 %s）: %s" % (
                        usb.get("port") or "?", str(exc)[:80])
                else:
                    ctrl["ir_state"] = "no_device"
                    ctrl["ir_error"] = "USB 线缆未连接（%s:%s 不在总线）" % (IR_VID, IR_PID)
            if not usb["present"]:
                stop_sdk()                        # 别让失败的进程占住设备
                time.sleep(3.0)                   # 3s 一轮盯着 USB 热插拔
            else:
                time.sleep(backoff)
                backoff = min(5.0, backoff * 1.5)
            continue
        time.sleep(0.03)


def ir_det_load():
    """端侧热成像检测器（ir_detect 服务）的最新结果。

    ir_detect 在 RV1126B 上跑两级检测：NPU 上的 YOLOv8（复用可见光模型，属跨模态
    复用，检出能力不等于可见光）+ 传统 CV 热斑。这里只读它的产物文件；
    文件过期就返回 None，宁可显示"没有结果"，也不给页面挂一份陈旧框。
    """
    try:
        st = os.stat(IR_DET_JSON)
        age = time.time() - st.st_mtime
        d = json.loads(open(IR_DET_JSON).read())
    except Exception:
        return None
    if age > IR_DET_STALE:
        return None
    d["age_s"] = round(age, 2)
    return d


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass


    def reply(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        raw = self.path.split("?")[0]
        qs = self.path.split("?")[1] if "?" in self.path else ""
        if raw == "/control":
            action = ""
            for kv in qs.split("&"):
                if kv.startswith("action="):
                    action = kv.split("=", 1)[1]
            if action not in ("start", "pause", "stop"):
                self.reply(400, "application/json", b'{"error":"bad action"}')
                return
            if action == "start":
                subprocess.run(["systemctl", "start", "yolo-live.service"], capture_output=True)
                subprocess.run(["systemctl", "start", "ir-detect.service"], capture_output=True)
                ensure_sdk()
                with lock:
                    ctrl["state"] = "running"
                    ctrl["last_change"] = "start"
            elif action == "pause":
                with lock:
                    ctrl["state"] = "paused"
                    ctrl["last_change"] = "pause"
            else:
                with lock:
                    ctrl["state"] = "stopped"
                    ctrl["last_change"] = "stop"
                stop_sdk()
                subprocess.run(["systemctl", "stop", "yolo-live.service"], capture_output=True)
                subprocess.run(["systemctl", "stop", "ir-detect.service"], capture_output=True)
            self.reply(200, "application/json",
                       json.dumps({"ok": True, "state": ctrl["state"]}).encode())
            return
        with lock:
            vis, ir = ctrl["vis"], ctrl["ir"]
            snap = {k: v for k, v in ctrl.items() if k not in
                    ("vis", "ir", "_vis_ts", "_ir_ts")}
        if raw == "/vis.jpg" and vis:
            self.reply(200, "image/jpeg", vis)
        elif raw == "/ir.png" and ir:
            self.reply(200, "image/png", ir)
        elif raw == "/status.json":
            snap["vis_ready"] = bool(vis)
            snap["ir_ready"] = bool(ir)
            snap["sdk_running"] = sdk_proc is not None and sdk_proc.poll() is None
            # 热成像自己实测的 USB 位置，作为 g4-guard 探测结果的兜底/交叉校验
            ir_link = ir_usb_probe()
            snap["ir_usb"] = ir_link["present"]
            snap["ir_link"] = ir_link
            snap["ir_vid_pid"] = "%s:%s" % (IR_VID, IR_PID)
            # 守护进程的探测结果：USB 视频设备解析 + 网口链路/中继实测
            for _src, _key in (("/tmp/g4-cam.json", "cams"),
                               ("/tmp/g4-net.json", "net")):
                try:
                    snap[_key] = json.loads(open(_src).read())
                except Exception:
                    snap[_key] = None
            try:
                snap["vis_dev"] = ((snap.get("cams") or {}).get("cams") or {}).get("vis", {}).get("node")
                snap["vis_alias"] = ((snap.get("cams") or {}).get("cams") or {}).get("vis", {}).get("alias")
            except Exception:
                snap["vis_dev"] = None
                snap["vis_alias"] = None
            snap["uptime_s"] = round(time.time() - ctrl["started_at"], 1)
            try:
                boxes_path = "/tmp/live_boxes.json"
                live = json.loads(open(boxes_path).read())
                # 检测结果同样要判新鲜度：可见光掉线后 yolo_live 不再写文件，
                # 旧框会一直挂在那儿，看着像"还在识别"。
                fresh = time.time() - os.stat(boxes_path).st_mtime < 3.0
                snap["detections"] = live.get("detections", []) if fresh else []
                snap["infer_ms"] = live.get("infer_ms") if fresh else None
                snap["yolo_seq"] = live.get("seq") if fresh else 0
                # 推理输入分辨率：框面积换算占比要用它，别让上层去猜 640×480
                snap["vis_res"] = live.get("res") if fresh else None
                # 这一路实际在跑哪个模型：由 yolo_live 自己上报，换模型重启即变，
                # 页面不做任何假设（后续要对比不同模型，就必须能动态看到）。
                snap["vis_model"] = ({
                    "name": live.get("model"), "path": live.get("model_path"),
                    "bytes": live.get("model_bytes"), "engine": "YOLOv8-RKNN",
                    "res": live.get("res"), "infer_ms": live.get("infer_ms"),
                } if fresh else None)
            except Exception:
                snap["detections"] = []
                snap["infer_ms"] = None
                snap["yolo_seq"] = 0
                snap["vis_res"] = None
                snap["vis_model"] = None
            # 热成像端侧检测结果（NPU 目标框 + 热斑），过期则显式报 no_data
            _ird = ir_det_load()
            snap["ir_det"] = _ird
            snap["ir_det_state"] = "running" if _ird else "no_data"
            snap["ir_detections"] = (_ird or {}).get("yolo", [])
            snap["ir_blobs"] = (_ird or {}).get("blobs", [])
            snap["ir_infer_ms"] = (_ird or {}).get("infer_ms")
            snap["ir_det_res"] = (_ird or {}).get("res")
            snap["ir_edge_engine"] = (_ird or {}).get("engine")
            snap["ir_model"] = ({
                "name": _ird.get("model"), "path": _ird.get("model_path"),
                "bytes": _ird.get("model_bytes"), "engine": _ird.get("engine"),
                "res": _ird.get("res"), "infer_ms": _ird.get("infer_ms"),
            } if _ird else None)
            self.reply(200, "application/json", json.dumps(snap, ensure_ascii=False).encode())
        elif raw in ("/", "/index.html"):
            self.reply(200, "text/html; charset=utf-8",
                       b"<h3>Camera preview</h3><p>/vis.jpg /ir.png /status.json /control</p>")
        else:
            self.reply(404, "text/plain", b"not found")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ensure_sdk()
    threading.Thread(target=vis_grabber, daemon=True).start()
    threading.Thread(target=ir_grabber, daemon=True).start()
    try:
        with Server((HOST, PORT), Handler) as srv:
            print("camera preview: http://%s:%d/" % (HOST, PORT), flush=True)
            srv.serve_forever()
    finally:
        stop_sdk()


if __name__ == "__main__":
    main()
