#!/usr/bin/env python3
"""G4 板端自动检测 / 建立 / 监测守护（RV1126B）

两条独立回路，各写一份状态文件供 dashboard 读取：

  1) USB 视频回路（每 5 s）
     - 扫 /sys/class/video4linux，按 USB VID:PID 识别设备，不依赖 /dev/videoN 编号
     - 维护稳定别名 /dev/g4-vis（可见光 1d6c:0103）、/dev/g4-ir（热成像 0bda:5840）
     - 采样每个节点的 index / 能力，判断"枚举了但没出图"
     - 输出 /tmp/g4-cam.json

  2) 网口回路（每 10 s）
     - eth0 上行交换机：carrier / 速率 / 192.168.77.22/24
     - eth1 下挂昇腾：三层中继（ip_forward + proxy ARP + rp_filter=0 + /32 主机路由）
     - 实测可达性：Mac .1 / 昇腾 .10
     - 缺什么补什么，动作写日志；输出 /tmp/g4-net.json

设计约束：
  - 只做"缺失才补"，不做周期性重配，避免把正在用的配置打断
  - 任何动作都记时间戳，便于回溯"什么时候自己修过一次"
  - 只依赖标准库（板端 python3.10，无第三方包）
"""
import json
import os
import subprocess
import sys
import threading
import time

CAMS = {
    "vis": {"vid": "1d6c", "pid": "0103", "alias": "/dev/g4-vis",
            "label": "可见光 UVC"},
    "ir": {"vid": "0bda", "pid": "5840", "alias": "/dev/g4-ir",
           "label": "热成像 UVC 桥"},
}

NET = {
    "up_if": "eth0",
    "up_cidr": "192.168.77.22/24",
    "up_ip": "192.168.77.22/24",
    "peer_if": "eth1",
    "relay_id": "192.168.77.24/32",
    "peer_ip": "192.168.77.10",
    "peer_cidr": "192.168.77.10/32",
    "uplink_ip": "192.168.77.1",
}

CAM_STATE = "/tmp/g4-cam.json"
NET_STATE = "/tmp/g4-net.json"
LOG = "/tmp/g4-guard.log"

_actions = []          # 最近动作（滚动 30 条）
_lock = threading.Lock()


def log(msg):
    line = "[%s] %s" % (time.strftime("%F %T"), msg)
    try:
        with open(LOG, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass
    print(line, flush=True)


def note(kind, msg):
    with _lock:
        _actions.append({"ts": int(time.time()), "ts_str": time.strftime("%F %T"),
                         "kind": kind, "msg": msg})
        del _actions[:-30]
    log("%s: %s" % (kind, msg))


def write_json(path, obj):
    tmp = path + ".tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(obj, f, ensure_ascii=False)
        os.replace(tmp, path)
    except OSError as exc:
        log("write %s failed: %s" % (path, exc))


def rd(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def run(cmd, timeout=6):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except Exception as exc:                     # noqa: BLE001
        return 1, str(exc)


# ─────────────────────────── USB 视频回路 ───────────────────────────

def usb_ids_of(devlink):
    """从 /sys/class/video4linux/videoN/device 的 realpath 向上找 idVendor/idProduct。"""
    cur = os.path.realpath(devlink)
    for _ in range(6):
        vid = rd(os.path.join(cur, "idVendor"))
        pid = rd(os.path.join(cur, "idProduct"))
        if vid and pid:
            return vid.lower(), pid.lower()
        parent = os.path.dirname(cur)
        if parent == cur or parent == "/":
            break
        cur = parent
    return "", ""


def usb_port_of(devlink):
    """解析设备接在哪个 USB 口。

    /sys/class/video4linux/videoN/device 的 realpath 形如
        .../usb1/1-1/1-1:1.0            （接口层，末尾带 :1.0）
    上一层 .../usb1/1-1 才是 USB 设备节点，端口号就是 "1-1"。
    返回 port / speed / busnum / devnum / product / controller。
    """
    info = {"port": None, "speed": None, "busnum": None, "devnum": None,
            "product": None, "controller": None}
    try:
        real = os.path.realpath(devlink)
    except OSError:
        return info
    devdir = real
    if ":" in os.path.basename(devdir):          # 去掉接口后缀 :1.0
        devdir = os.path.dirname(devdir)
    if not rd(os.path.join(devdir, "busnum")):
        parent = os.path.dirname(devdir)
        if rd(os.path.join(parent, "busnum")):
            devdir = parent
    if not rd(os.path.join(devdir, "busnum")):
        return info
    info["port"] = os.path.basename(devdir)
    info["busnum"] = rd(os.path.join(devdir, "busnum"))
    info["devnum"] = rd(os.path.join(devdir, "devnum"))
    info["speed"] = rd(os.path.join(devdir, "speed"))
    info["product"] = rd(os.path.join(devdir, "product"))
    # 控制器（根 hub）名，用来区分同一个端口号落在哪个控制器上
    parts = devdir.split("/")
    for i, seg in enumerate(parts):
        if seg.startswith("usb") and seg[3:].isdigit():
            info["controller"] = seg
            break
    return info


def scan_video():
    base = "/sys/class/video4linux"
    try:
        names = sorted(os.listdir(base))
    except OSError:
        return []
    out = []
    for name in names:
        if not name.startswith("video"):
            continue
        d = os.path.join(base, name)
        try:
            index = int(rd(os.path.join(d, "index"), "-1") or -1)
        except ValueError:
            index = -1
        vid, pid = usb_ids_of(os.path.join(d, "device"))
        entry = {
            "node": "/dev/" + name,
            "index": index,
            "card": rd(os.path.join(d, "name")),
            "vid": vid,
            "pid": pid,
            "usb": bool(vid and pid),
        }
        if entry["usb"]:
            entry.update(usb_port_of(os.path.join(d, "device")))
        out.append(entry)
    return out


USB_DEVS = "/sys/bus/usb/devices"


def _iface_drivers(devdir):
    """设备下各接口的驱动绑定：{"1.0": "usbfs", "1.1": "uvcvideo"}。"""
    out = {}
    try:
        names = os.listdir(devdir)
    except OSError:
        return out
    for n in names:
        if ":" not in n:
            continue
        link = os.path.join(devdir, n, "driver")
        # 没有 driver 链接 = 该接口当前没有驱动绑定
        out[n.split(":", 1)[1]] = (os.path.basename(os.path.realpath(link))
                                   if os.path.exists(link) else None)
    return out


def usb_by_ids(vid, pid):
    """直接扫 USB 总线按 VID:PID 定位设备，不经过 video4linux。

    为什么需要这条路径：热成像这类模组常被厂商 SDK 用 usbfs 方式独占（libusb
    claim 接口），uvcvideo 拿不到接口就不会创建 /dev/video* 节点。此时"线插没插"
    只能从 USB 层判断——否则设备明明在总线上出着图，界面却报"模组未枚举"。
    """
    try:
        names = os.listdir(USB_DEVS)
    except OSError:
        return None
    for n in names:
        if ":" in n:                       # 跳过 4-1.2:1.0 这类接口节点
            continue
        d = os.path.join(USB_DEVS, n)
        if rd(os.path.join(d, "idVendor")).lower() != vid:
            continue
        if rd(os.path.join(d, "idProduct")).lower() != pid:
            continue
        ifaces = _iface_drivers(d)
        bound = {v for v in ifaces.values() if v}
        if "usbfs" in bound:
            mode = "usbfs"                 # 用户态进程直接占用（厂商 SDK 独占）
        elif "uvcvideo" in bound:
            mode = "v4l2"
        else:
            mode = "usb_only"              # 枚举了但没有驱动绑定
        busnum = rd(os.path.join(d, "busnum"))
        return {
            "port": n,                     # sysfs 目录名即 busnum-devpath，如 4-1.2 / 1-1
            "speed": rd(os.path.join(d, "speed")),
            "busnum": busnum,
            "devnum": rd(os.path.join(d, "devnum")),
            "product": rd(os.path.join(d, "product")),
            "manufacturer": rd(os.path.join(d, "manufacturer")),
            "serial": rd(os.path.join(d, "serial")),
            "controller": ("usb" + busnum) if busnum else None,
            "mode": mode,
            "ifaces": ifaces,
        }
    return None


def ensure_alias(alias, target):
    """原子维护稳定别名，返回 (ok, changed)。"""
    try:
        if os.path.islink(alias) and os.path.realpath(alias) == os.path.realpath(target):
            return True, False
        tmp = alias + ".new"
        try:
            os.unlink(tmp)
        except OSError:
            pass
        os.symlink(target, tmp)
        os.replace(tmp, alias)
        return True, True
    except OSError as exc:
        note("cam", "建别名 %s -> %s 失败: %s" % (alias, target, exc))
        return False, False


_cam_last = {}          # key -> 上一次的 state，只在状态变化时记一条日志


def cam_loop(interval=5):
    while True:
        try:
            nodes = scan_video()
            result = {}
            for key, spec in CAMS.items():
                hit = [n for n in nodes
                       if n["vid"] == spec["vid"] and n["pid"] == spec["pid"]]
                # UVC 的 index0 是采集节点，index1 通常是 metadata
                capture = [n for n in hit if n["index"] == 0] or hit
                entry = {
                    "label": spec["label"],
                    "vid_pid": "%s:%s" % (spec["vid"], spec["pid"]),
                    "alias": spec["alias"],
                    # usb = 设备是否在 USB 总线上，与有没有 /dev/video 节点无关
                    "usb": False,
                    "bus_enum": False,
                    "node": None,
                    "nodes": [n["node"] for n in hit],
                    "card": capture[0]["card"] if capture else None,
                    # 接入位置：USB 端口号 / 速率 / 总线号，供控制台直接展示
                    "port": None, "speed": None, "busnum": None, "devnum": None,
                    "product": None, "controller": None, "mode": None, "ifaces": None,
                }
                if capture:
                    # ① 常规 UVC：有 /dev/video 节点，可直接 V4L2 抓流
                    node = capture[0]["node"]
                    entry["state"] = "v4l2"
                    entry["bus_enum"] = entry["usb"] = True
                    entry["mode"] = "v4l2"
                    for f in ("port", "speed", "busnum", "devnum", "product", "controller"):
                        entry[f] = capture[0].get(f)
                    entry["node"] = node
                    exists = os.path.exists(node)
                    entry["node_exists"] = exists
                    if exists:
                        ok, changed = ensure_alias(spec["alias"], node)
                        entry["alias_ok"] = ok
                        if changed:
                            note("cam", "%s 别名 %s -> %s" % (spec["label"], spec["alias"], node))
                    else:
                        entry["node_exists"] = False
                else:
                    # ② 没有 video 节点：回退到 USB 总线判断线到底插没插。
                    #    厂商 SDK 以 usbfs 独占时会走到这里——设备在、口知道、但没节点。
                    usb = usb_by_ids(spec["vid"], spec["pid"])
                    if usb:
                        entry["usb"] = entry["bus_enum"] = True
                        entry["state"] = usb["mode"]
                        for f in ("port", "speed", "busnum", "devnum",
                                  "product", "controller", "mode", "ifaces"):
                            entry[f] = usb.get(f)
                        # usbfs 独占时不建 /dev/g4-ir：那是给 V4L2 用的，
                        # 指向不存在的节点只会让下游打开失败。
                        entry["node_exists"] = False
                    else:
                        entry["state"] = "absent"
                    # 设备不可用：清掉悬空别名，避免下游打开一个死链接
                    if os.path.islink(spec["alias"]):
                        try:
                            os.unlink(spec["alias"])
                            note("cam", "%s 掉线，移除别名 %s" % (spec["label"], spec["alias"]))
                        except OSError:
                            pass

                if _cam_last.get(key) != entry["state"]:
                    _cam_last[key] = entry["state"]
                    if entry["state"] == "v4l2":
                        note("cam", "%s 就绪 %s（口 %s）"
                             % (spec["label"], entry["node"], entry["port"]))
                    elif entry["state"] == "usbfs":
                        note("cam", "%s 已枚举（口 %s），被厂商 SDK 以 usbfs 独占，无 /dev/video 节点"
                             % (spec["label"], entry["port"]))
                    elif entry["state"] == "usb_only":
                        note("cam", "%s 已枚举（口 %s）但无驱动绑定"
                             % (spec["label"], entry["port"]))
                    else:
                        note("cam", "%s 不在 USB 总线（线缆/供电未连接）" % spec["label"])
                result[key] = entry

            write_json(CAM_STATE, {
                "ts": int(time.time()),
                "ts_str": time.strftime("%F %T"),
                "all_nodes": nodes,
                "cams": result,
                "actions": list(_actions)[-10:],
            })
        except Exception as exc:                 # noqa: BLE001
            log("cam_loop error: %s" % exc)
        time.sleep(interval)


# ─────────────────────────── 网口回路 ───────────────────────────

def iface_info(dev):
    base = "/sys/class/net/" + dev
    if not os.path.exists(base):
        return {"present": False}
    carrier = rd(os.path.join(base, "carrier"), "")
    speed = rd(os.path.join(base, "speed"), "")
    info = {
        "present": True,
        "carrier": carrier == "1",
        "speed_mbps": int(speed) if speed.isdigit() else None,
        "mac": rd(os.path.join(base, "address")),
        "state": rd(os.path.join(base, "operstate")),
        "driver": "",
        "ipv4": [],
    }
    uevent = rd(os.path.join(base, "device/uevent"))
    for line in uevent.splitlines():
        if line.startswith("DRIVER="):
            info["driver"] = line.split("=", 1)[1]
            break
    rc, out = run(["ip", "-4", "-o", "addr", "show", "dev", dev])
    if rc == 0:
        for line in out.splitlines():
            parts = line.split()
            if "inet" in parts:
                i = parts.index("inet")
                if i + 1 < len(parts):
                    info["ipv4"].append(parts[i + 1])
    return info


def has_addr(dev, cidr):
    dev_info = iface_info(dev)
    return cidr in dev_info.get("ipv4", [])


def has_route(target_cidr, dev):
    rc, out = run(["ip", "route", "show", "dev", dev])
    if rc != 0:
        return False
    return target_cidr.split("/")[0] in out


def sysctl_get(key):
    return rd("/proc/sys/" + key.replace(".", "/"))


def sysctl_set(key, val):
    cur = sysctl_get(key)
    if cur == str(val):
        return False
    rc, out = run(["sysctl", "-q", "-w", "%s=%s" % (key, val)])
    if rc == 0:
        note("net", "sysctl %s %s -> %s" % (key, cur or "?", val))
        return True
    note("net", "sysctl %s=%s 失败: %s" % (key, val, out.strip()[:80]))
    return False


def ping_ok(ip, timeout=1):
    rc, _ = run(["ping", "-c", "1", "-W", str(timeout), ip], timeout=timeout + 3)
    return rc == 0


def net_loop(interval=10):
    while True:
        try:
            up = iface_info(NET["up_if"])
            peer = iface_info(NET["peer_if"])
            repairs = []

            # 1) 上行口地址（缺失才补，不动已有配置）
            if up.get("present") and up.get("carrier"):
                if not has_addr(NET["up_if"], NET["up_ip"]):
                    rc, out = run(["ip", "addr", "add", NET["up_cidr"], "dev", NET["up_if"]])
                    if rc == 0:
                        note("net", "%s 补地址 %s" % (NET["up_if"], NET["up_cidr"]))
                        repairs.append("up_addr")
                    else:
                        note("net", "%s 补地址失败: %s" % (NET["up_if"], out.strip()[:80]))

            # 2) 中继开关（只在有下挂口时维护）
            if peer.get("present"):
                for key, val in (("net.ipv4.ip_forward", "1"),
                                 ("net.ipv4.conf.%s.proxy_arp" % NET["up_if"], "1"),
                                 ("net.ipv4.conf.%s.proxy_arp" % NET["peer_if"], "1"),
                                 ("net.ipv4.conf.%s.rp_filter" % NET["up_if"], "0"),
                                 ("net.ipv4.conf.%s.rp_filter" % NET["peer_if"], "0")):
                    if sysctl_set(key, val):
                        repairs.append(key)

                if peer.get("carrier"):
                    if not has_addr(NET["peer_if"], NET["relay_id"]):
                        rc, out = run(["ip", "addr", "add", NET["relay_id"],
                                       "dev", NET["peer_if"]])
                        if rc == 0:
                            note("net", "%s 补中继标识地址 %s" % (NET["peer_if"], NET["relay_id"]))
                            repairs.append("relay_id")
                    if not has_route(NET["peer_cidr"], NET["peer_if"]):
                        rc, out = run(["ip", "route", "replace", NET["peer_cidr"],
                                       "dev", NET["peer_if"]])
                        if rc == 0:
                            note("net", "%s 补主机路由 %s" % (NET["peer_if"], NET["peer_cidr"]))
                            repairs.append("peer_route")

            # 3) 实测可达性（这是"连接是否真的建立"的唯一硬证据）
            reach_up = ping_ok(NET["uplink_ip"]) if up.get("carrier") else False
            reach_peer = ping_ok(NET["peer_ip"]) if peer.get("carrier") else False

            write_json(NET_STATE, {
                "ts": int(time.time()),
                "ts_str": time.strftime("%F %T"),
                "uplink": dict(up, role="uplink(switch)", reachable=reach_up,
                               peer=NET["uplink_ip"]),
                "downlink": dict(peer, role="relay(ascend)", reachable=reach_peer,
                                 peer=NET["peer_ip"]),
                "relay": {
                    "ip_forward": sysctl_get("net.ipv4.ip_forward"),
                    "proxy_arp_up": sysctl_get("net.ipv4.conf.%s.proxy_arp" % NET["up_if"]),
                    "proxy_arp_peer": sysctl_get("net.ipv4.conf.%s.proxy_arp" % NET["peer_if"]),
                },
                "repairs_last_cycle": repairs,
                "actions": list(_actions)[-10:],
            })
        except Exception as exc:                 # noqa: BLE001
            log("net_loop error: %s" % exc)
        time.sleep(interval)


def main():
    log("g4-guard start (pid %d)" % os.getpid())
    threading.Thread(target=cam_loop, daemon=True).start()
    threading.Thread(target=net_loop, daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    if os.geteuid() != 0:
        sys.exit("need root")
    main()
