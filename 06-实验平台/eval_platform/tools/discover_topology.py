#!/usr/bin/env python3
"""G4 拓扑自动发现 —— 实测每台设备的物理接口、连接方式、链路速率与二层形状。

    python3 eval_platform/tools/discover_topology.py            # 打印摘要
    python3 eval_platform/tools/discover_topology.py --json     # 输出完整 JSON
    python3 eval_platform/tools/discover_topology.py --write    # 写入 dashboard/topology_live.json

口径（重要）：
  1. 接口名 / MAC / 载波 / 速率 / 驱动 全部来自设备 sysfs 与 iproute2 实测，不猜。
  2. 「连接方式」（板载 GMAC / USB 转以太网 / PCIe 网卡 / Wi-Fi）由 driver 名 +
     sysfs 总线类型判定，并记录判定依据（media_basis）。
  3. 交换机本身不可管理（无 SSH、无 SNMP），因此「某台设备插在交换机哪个口」
     无法直接读取。star/chained 形状由「是否存在 bridge/转发/双上联」+ 可达矩阵推断，
     置信度标 inferred，不冒充实测。
  4. 设备离线时保留节点并标注 offline + 原因，不删除、不伪造接口数据。
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
DASH = HERE.parent / "dashboard"
OUT = DASH / "topology_live.json"

SUBNET = "192.168.77.0/24"
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
            "-o", "StrictHostKeyChecking=no"]

# ── 设备清单：id -> 展示信息 + 探测方式 ──────────────────────────────────────
DEVICES = [
    {"id": "mac", "label": "MacBook Air M1", "kind": "control", "ip": "192.168.77.1",
     "ssh": None, "role": "显示 / 控制（无推理）"},
    {"id": "ascend", "label": "昇腾 QiZhi01-20T", "kind": "compute", "ip": "192.168.77.10",
     "ssh": "ascend", "role": "中心 LLM（310B1 / 24 GB）"},
    {"id": "rk3588", "label": "RK3588 LubanCat", "kind": "edge", "ip": "192.168.77.21",
     "ssh": "rk3588", "role": "端侧视觉 / 事件复核"},
    {"id": "rv1126b", "label": "RV1126B Nano-TB", "kind": "edge", "ip": "192.168.77.22",
     "ssh": "rv1126b", "role": "三摄采集 / YOLOv8n"},
]

# ── 介质判定：driver 片段 -> (介质, 说明) ────────────────────────────────────
DRIVER_MEDIA = [
    (("r8152", "r8153"), "USB 转以太网", "Realtek RTL8152/8153 USB 网卡"),
    (("ax88179", "ax8817", "ax_usb"), "USB 转以太网", "ASIX AX88179 USB 网卡"),
    (("cdc_ncm", "cdc_ether", "cdc_subset"), "USB 转以太网", "CDC ECM/NCM USB 网卡"),
    (("dwmac", "stmmac", "gmac", "macb", "dwc_ether", "dwc-qos"), "SoC 板载 GMAC", "SoC 内置 MAC"),
    (("hns3", "hns"), "SoC 板载 GMAC", "海思 hns 以太网"),
    (("r8169", "r8168", "e1000", "igb", "ixgbe", "atlantic", "tg3"), "PCIe 以太网", "独立以太网控制器"),
]
USB_MARKERS = ("usb",)

# 这些不是可接线的以太网口：CAN / dummy / Wi-Fi P2P / bond / USB gadget / 隧道
SKIP_IF = re.compile(
    r"^(can\d|dummy\d|Featureid|p2p\d|bond\d|usb\d|sit\d|tun\d|tap\d|veth|docker|br-|virbr|"
    r"hwsim|ip6tnl|erspan|nlmon|wfdiag|rmnet)", re.I)


def run(cmd, timeout=12, stdin_data=None):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           input=stdin_data)
        return r.returncode, r.stdout or "", r.stderr or ""
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"
    except Exception as exc:  # noqa: BLE001
        return 1, "", str(exc)


# ── 远端采集脚本（纯 shell，不依赖 python）──────────────────────────────────
REMOTE_SH = r"""
echo "##HOST"; hostname 2>/dev/null
echo "##UPTIME"; cut -d' ' -f1 /proc/uptime 2>/dev/null
echo "##IF"
for i in /sys/class/net/*; do
  n=$(basename "$i")
  case "$n" in lo|bonding_masters|veth*|docker*|br-*|virbr*) continue;; esac
  [ -d "$i" ] || continue
  mac=$(cat "$i/address" 2>/dev/null)
  oper=$(cat "$i/operstate" 2>/dev/null)
  car=$(cat "$i/carrier" 2>/dev/null)
  spd=$(cat "$i/speed" 2>/dev/null)
  mtu=$(cat "$i/mtu" 2>/dev/null)
  drv=$(grep -m1 '^DRIVER=' "$i/device/uevent" 2>/dev/null | cut -d= -f2)
  sub=$(readlink -f "$i/device/subsystem" 2>/dev/null | sed 's#.*/##')
  typ=$(cat "$i/type" 2>/dev/null)
  echo "$n|$mac|$oper|$car|$spd|$mtu|$drv|$sub|$typ"
done
echo "##ADDR"; ip -4 -o addr show 2>/dev/null
echo "##ROUTE"; ip route 2>/dev/null
echo "##NEIGH"; ip neigh 2>/dev/null
echo "##FWD"; cat /proc/sys/net/ipv4/ip_forward 2>/dev/null
echo "##BRIDGE"; ip -d link show type bridge 2>/dev/null | grep -oE "^[0-9]+: [^:]+" | sed 's/^[0-9]*: //'
echo "##PROXYARP"
for f in /proc/sys/net/ipv4/conf/*/proxy_arp; do
  n=$(basename "$(dirname "$f")")
  echo "$n $(cat "$f" 2>/dev/null)"
done
echo "##END"
"""


def parse_remote(text):
    """把 ##SECTION 分隔的输出切成 dict[section] -> list[str]。"""
    sec, buf = None, {}
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("##"):
            sec = s[2:]
            buf.setdefault(sec, [])
            continue
        if sec and s:
            buf[sec].append(line.rstrip())
    return buf


# ── 接口解析 ─────────────────────────────────────────────────────────────────
def classify_media(driver, subsystem, name, speed):
    """返回 (介质, 依据)。"""
    d = (driver or "").lower()
    if name.startswith(("wlan", "wlp", "wl")):
        return "Wi-Fi", f"接口名 {name}"
    for keys, media, note in DRIVER_MEDIA:
        if any(k in d for k in keys):
            extra = f"driver={driver}"
            if subsystem:
                extra += f", 总线={subsystem}"
            return media, extra
    if (subsystem or "").lower() in USB_MARKERS:
        return "USB 转以太网", f"sysfs 总线=usb, driver={driver or '?'}"
    if (subsystem or "").lower() == "pci":
        return "PCIe 以太网", f"sysfs 总线=pci, driver={driver or '?'}"
    if d:
        return "以太网（未识别型号）", f"driver={driver}"
    return "未知接口", "无 driver 信息"


def norm_speed(raw):
    """sysfs speed: 1000 / -1 / 空。返回 int|None。"""
    try:
        v = int(str(raw).strip())
        return v if v > 0 else None
    except Exception:  # noqa: BLE001
        return None


def iface_from_line(line):
    parts = line.split("|")
    if len(parts) < 9:
        return None
    name, mac, oper, car, spd, mtu, drv, sub, typ = parts[:9]
    if SKIP_IF.match(name):
        return None
    speed = norm_speed(spd)
    media, basis = classify_media(drv, sub, name, speed)
    try:
        itype = int(typ)
    except Exception:  # noqa: BLE001
        itype = None
    is_wifi = name.startswith(("wlan", "wlp", "wl"))
    carried = car.strip() == "1"
    # 无载波时 sysfs 的 speed 是历史残留值（如 RV1126B eth1 报 10M 实际未插线）
    eff_speed = speed if carried else None
    return {
        "name": name,
        "mac": mac or None,
        "state": (oper or "unknown").upper(),
        "carrier": carried,
        "speed_mbps": eff_speed,
        "speed_raw": speed,
        "mtu": int(mtu) if str(mtu).strip().isdigit() else None,
        "driver": drv or None,
        "bus": sub or None,
        "media": media,
        "media_basis": basis,
        "kind": "wifi" if is_wifi else ("ethernet" if itype == 1 else "other"),
        "virtual": bool(re.match(r"^(bond|dummy|can|usb\d|sit|tun|tap)", name)),
    }


def parse_addr(lines):
    """ip -4 -o addr show -> {ifname: [cidr, ...]}"""
    out = {}
    for line in lines:
        m = re.match(r"^\d+:\s+(\S+)\s+inet\s+(\S+)", line.strip())
        if m:
            out.setdefault(m.group(1), []).append(m.group(2))
    return out


def parse_route(lines):
    routes = []
    for line in lines:
        line = line.strip()
        if line:
            routes.append(line)
    return routes


def parse_neigh(lines):
    neigh = []
    for line in lines:
        p = line.split()
        if len(p) < 3:
            continue
        rec = {"ip": p[0], "dev": p[2] if len(p) > 2 else None}
        if "lladdr" in p:
            rec["mac"] = p[p.index("lladdr") + 1]
        rec["state"] = p[-1]
        neigh.append(rec)
    return neigh


# ── 本机（macOS）采集 ────────────────────────────────────────────────────────
def collect_mac():
    dev = {"id": "mac"}
    rc, out, err = run(["ifconfig"], timeout=10)
    if rc != 0:
        return {**dev, "online": False, "error": err or "ifconfig failed"}

    # 硬件端口名（判断 USB 网卡）
    hw = {}
    rc2, o2, _ = run(["networksetup", "-listallhardwareports"], timeout=10)
    if rc2 == 0:
        cur = None
        for line in o2.splitlines():
            m = re.match(r"^Hardware Port:\s*(.+)$", line)
            if m:
                cur = m.group(1).strip()
            m2 = re.match(r"^Device:\s*(\S+)", line)
            if m2 and cur:
                hw[m2.group(1)] = cur

    ifaces = []
    cur = None
    buf = []
    blocks = []
    for line in out.splitlines():
        if re.match(r"^\S+:", line):
            if cur:
                blocks.append((cur, buf))
            cur = line.split(":")[0]
            buf = [line]
        elif cur:
            buf.append(line)
    if cur:
        blocks.append((cur, buf))

    for name, lines in blocks:
        if name == "lo0" or name.startswith(("gif", "stf", "bridge", "utun", "awdl", "llw", "ap1", "anpi")):
            continue
        blob = "\n".join(lines)
        mac = None
        m = re.search(r"ether\s+([0-9a-f:]{17})", blob)
        if m:
            mac = m.group(1)
        if not mac:          # 无 MAC 的不是可用网口（如 p2p/内部隧道）
            continue
        ips = re.findall(r"inet\s+(\d+\.\d+\.\d+\.\d+)(?:\s+netmask\s+(\S+))?", blob)
        up = "UP" in blob.split("\n")[0]
        running = "RUNNING" in blob.split("\n")[0]
        media_txt, speed = None, None
        mm = re.search(r"media:\s*(.+)", blob)
        if mm:
            media_txt = mm.group(1).strip()
            sm = re.search(r"(\d+)base", media_txt)
            if sm:
                speed = int(sm.group(1))
            elif "1000" in media_txt:
                speed = 1000
        status = None
        ms = re.search(r"status:\s*(\w+)", blob)
        if ms:
            status = ms.group(1)
        # 无载波、无地址、media=none 的端口不是实际接线口（Thunderbolt/内部适配器）
        active = (status == "active")
        if not active and not ips and (media_txt in (None, "", "none")):
            continue
        port = hw.get(name, "")
        low = (port or "").lower()
        if any(k in low for k in ("ax8817", "ax88179", "rtl815", "usb", "lan")):
            media = "USB 转以太网"
        elif "thunderbolt" in low:
            media = "Thunderbolt 扩展网卡"
        elif "wi-fi" in low or "airport" in low:
            media = "Wi-Fi"
        elif port:
            media = "以太网（板载）"
        else:
            media = "以太网（未识别）"
        basis = f"networksetup 硬件端口=「{port or '?'}」"
        if media_txt:
            basis += f", media={media_txt}"
        plain_ips = [i[0] for i in ips]
        ifaces.append({
            "name": name, "mac": mac,
            "ipv4": plain_ips,
            "ipv4_raw": plain_ips,
            "state": "UP" if up and running else ("UP" if up else "DOWN"),
            "carrier": active,
            "speed_mbps": speed,
            "mtu": None, "driver": None, "bus": "usb" if "usb" in media else None,
            "media": media, "media_basis": basis,
            "kind": "wifi" if media == "Wi-Fi" else "ethernet",
            "virtual": False,
            "link_status": status,
            "hardware_port": port or None,
        })

    rc3, arp_out, _ = run(["arp", "-an"], timeout=8)
    neigh = []
    for line in arp_out.splitlines():
        m = re.match(r"\S+\s+\((\d+\.\d+\.\d+\.\d+)\)\s+at\s+([0-9a-f:]+)\s+on\s+(\S+)", line)
        if m:
            neigh.append({"ip": m.group(1), "mac": m.group(2), "dev": m.group(3),
                          "state": "REACHABLE"})

    rc4, route_out, _ = run(["netstat", "-rn"], timeout=8)
    routes = [l.strip() for l in route_out.splitlines()
              if l.strip().startswith(("default", "192.168.77"))]

    return {
        **dev, "online": True, "hostname": "macbook-air-m1",
        "interfaces": ifaces, "neigh": neigh, "routes": routes,
        "forwarding": {"ip_forward": 0, "bridges": [], "proxy_arp": {}},
        "raw_ok": True,
    }


# ── 远端设备采集 ─────────────────────────────────────────────────────────────
def collect_device(dev):
    base = {"id": dev["id"], "label": dev["label"], "kind": dev["kind"],
            "ip": dev["ip"], "role": dev["role"]}
    if dev["id"] == "mac":
        mac = collect_mac()
        return {**base, **{k: v for k, v in mac.items() if k != "id"}}

    if not dev.get("ssh"):
        return {**base, "online": False, "probe": {"method": "none", "ok": False,
                                                   "error": "未配置探测通道"}}

    t0 = time.time()
    # 脚本经 stdin 送进远端 bash，避免多行脚本被参数拼接破坏
    rc, out, err = run(["ssh", *SSH_OPTS, dev["ssh"], "bash -s"],
                       timeout=30, stdin_data=REMOTE_SH)
    dt = time.time() - t0
    if rc != 0 or "##IF" not in out:
        return {**base, "online": False,
                "probe": {"method": "ssh", "ok": False, "alias": dev["ssh"],
                          "latency_s": round(dt, 2),
                          "error": (err.strip().splitlines() or ["ssh failed"])[-1][:180]}}

    sec = parse_remote(out)
    ifaces = []
    for line in sec.get("IF", []):
        it = iface_from_line(line)
        if it and it["kind"] in ("ethernet", "wifi"):
            ifaces.append(it)
    addr = parse_addr(sec.get("ADDR", []))
    for it in ifaces:
        it["ipv4"] = addr.get(it["name"], [])

    fwd_raw = (sec.get("FWD") or ["0"])[0].strip()
    proxy = {}
    for line in sec.get("PROXYARP", []):
        p = line.split()
        if len(p) == 2 and p[1].strip().isdigit():
            proxy[p[0]] = int(p[1])
    return {
        **base, "online": True,
        "hostname": (sec.get("HOST") or [""])[0].strip() or None,
        "uptime_s": int(float((sec.get("UPTIME") or ["0"])[0] or 0)),
        "interfaces": ifaces,
        "neigh": parse_neigh(sec.get("NEIGH", [])),
        "routes": parse_route(sec.get("ROUTE", [])),
        "forwarding": {"ip_forward": 1 if fwd_raw == "1" else 0,
                       "bridges": [b.strip() for b in sec.get("BRIDGE", []) if b.strip()],
                       "proxy_arp": proxy},
        "probe": {"method": "ssh", "ok": True, "alias": dev["ssh"],
                  "latency_s": round(dt, 2)},
    }


# ── 拓扑推断 ─────────────────────────────────────────────────────────────────
def lab_iface(dev):
    """挑出参与实验网段的物理接口。

    优先：带 192.168.77.x 地址且非虚拟；
    退化：有载波的以太网口（刚断线、地址被撤时仍能定位到物理口）；
    再退化：USB/板载以太网口（用于"线拔了"这种状态）。
    """
    ifaces = [it for it in dev.get("interfaces", [])
              if not it.get("virtual") and it.get("kind") == "ethernet"]
    best = None
    for it in ifaces:
        ips = [str(ip) for ip in (it.get("ipv4") or [])]
        if not any(ip.startswith("192.168.77.") for ip in ips):
            continue
        # 带 /24 掩码的口才是真正"在实验网段里"的口；/32 通常是中转/中继口
        has24 = any(re.match(r"^192\.168\.77\.\d+/24$", ip) for ip in ips)
        score = ((4 if has24 else 0) + (2 if it.get("carrier") else 0)
                 + (1 if it.get("state") == "UP" else 0))
        if best is None or score > best[0]:
            best = (score, it)
    if best:
        return best[1]
    for it in ifaces:
        if it.get("carrier"):
            return it
    for it in ifaces:
        if it.get("media") in ("USB 转以太网", "SoC 板载 GMAC", "PCIe 以太网"):
            return it
    return None


def ping_from_mac(ip):
    rc, out, _ = run(["ping", "-c", "1", "-t", "2", ip], timeout=4)
    if rc != 0 or "100.0% packet loss" in out or "round-trip" not in out:
        return None
    m = re.search(r"=\s*[\d.]+/([\d.]+)/", out)
    return round(float(m.group(1)), 2) if m else 0.0


def find_relay_links(devs):
    """识别「设备 A 经设备 B 三层中继挂在其网口下」的关系。

    判定依据全部来自实测，三条同时满足才认定：
      1. B 的路由表里有 `<A.ip>/32 dev <iface>` 主机路由 → B 明确把 A 当作此口下挂主机；
      2. B 的邻居表在该 iface 上学到 A.ip 的 lladdr → 对端确实连在这块口的二层上；
      3. B 的 ip_forward=1 且该口 proxy_arp=1 → B 有能力代答 ARP 完成中继。
    缺任一条不认定（宁可不画，不伪造对端）。
    """
    by_ip = {d.get("ip"): d for d in devs if d.get("ip")}
    found, seen = [], set()
    for parent in devs:
        if not parent.get("online"):
            continue
        fw = parent.get("forwarding") or {}
        proxy = fw.get("proxy_arp") or {}
        for rt in (parent.get("routes") or []):
            # `ip route` 对单主机路由不打印 /32 后缀，两种写法都要认
            m = re.match(r"^(\d+\.\d+\.\d+\.\d+)(?:/32)?\s+dev\s+(\S+)", str(rt).strip())
            if not m:
                continue
            tip, pif = m.group(1), m.group(2)
            child = by_ip.get(tip)
            if not child or child["id"] == parent["id"] or child["id"] in seen:
                continue
            nb = [n for n in (parent.get("neigh") or [])
                  if n.get("ip") == tip and n.get("mac")]
            if not nb:
                continue
            iface = next((it for it in parent.get("interfaces", [])
                          if it["name"] == pif), None)
            if not iface or iface.get("kind") != "ethernet":
                continue
            relay_ok = bool(fw.get("ip_forward")) and bool(proxy.get(pif))
            evidence = [
                f"{parent['id']} 路由表含 {tip}/32 dev {pif} → 明确把该主机当作此口下挂",
                f"{parent['id']} 在 {pif} 上学到 {tip} 的 MAC {nb[0]['mac']} → 对端确在此口二层",
            ]
            if relay_ok:
                evidence.append(f"{parent['id']} ip_forward=1 + {pif}.proxy_arp=1 "
                                f"→ 由它代答 ARP 做三层中继（同一广播域被拆成两段）")
            else:
                evidence.append(f"⚠ {parent['id']} ip_forward={fw.get('ip_forward')}、"
                                f"{pif}.proxy_arp={proxy.get(pif)} → 路由在但中继未启用，链路可能不通")
            seen.add(child["id"])
            found.append({
                "parent": parent["id"], "parent_if": pif,
                "child": child["id"], "child_ip": tip, "child_mac": nb[0]["mac"],
                "relay": relay_ok, "evidence": evidence,
            })
    return found


def build_topology(devs):
    by_id = {d["id"]: d for d in devs}

    # 1) 实验接口 + 介质
    for d in devs:
        li = lab_iface(d)
        d["lab_interface"] = li["name"] if li else None
        d["lab_media"] = li["media"] if li else None
        d["lab_speed_mbps"] = li["speed_mbps"] if li else None
        d["lab_mac"] = li["mac"] if li else None

    # 1.5) 三层中继挂载关系：某台设备被另一台设备的网口"接管"（同广播域被拆成两段）
    relay_links = find_relay_links(devs)
    relay_by_child = {r["child"]: r for r in relay_links}
    for d in devs:
        r = relay_by_child.get(d["id"])
        if r:
            d["uplink"] = {"type": "relay", "via": r["parent"], "via_if": r["parent_if"],
                           "relay_enabled": r["relay"], "evidence": r["evidence"]}
        else:
            d["uplink"] = {"type": "switch"}

    # 2) 端口角色：实验口 / 中继口（下挂子设备）/ 闲置口
    #    中继口虽无 /24 地址，但已经在承载下挂设备，不能算"闲置"——
    #    否则会误报成"有载波却没用上"的黄灯。
    relay_port = {}
    for r in relay_links:
        relay_port.setdefault(r["parent"], {}).setdefault(r["parent_if"], []).append(r["child"])
    for d in devs:
        idle = []
        for it in d.get("interfaces", []):
            if it.get("virtual") or it.get("kind") == "other":
                continue
            kids = (relay_port.get(d["id"]) or {}).get(it["name"])
            ips = [str(ip) for ip in (it.get("ipv4") or [])]
            if kids:
                it["port_role"] = "relay"
                it["serves"] = kids
                continue                      # 中继口 = 在用端口，不进闲置列表
            if it["name"] == d.get("lab_interface"):
                it["port_role"] = "lab"
                continue
            if it.get("kind") == "wifi":
                it["port_role"] = "wifi"
                continue
            it["port_role"] = "idle"
            idle.append({"name": it["name"], "media": it["media"],
                         "state": it["state"], "carrier": it.get("carrier"),
                         "speed_mbps": it.get("speed_mbps"),
                         "relay_port": False,
                         "note": "有载波但未配实验地址" if it.get("carrier") else "未插线"})
        d["idle_ports"] = idle

    # 3) 可达矩阵：从 Mac 对每台设备实测 RTT（既有 status 复用）
    reach = {}
    for d in devs:
        if d["id"] == "mac":
            continue
        reach[d["id"]] = ping_from_mac(d["ip"]) if d.get("online") else None

    # 3.5) 未纳管设备：Mac ARP 表里属于实验网段、但不在清单中的条目
    known_ips = {d["ip"] for d in devs}
    unmanaged = []
    mac_dev = next((d for d in devs if d["id"] == "mac"), {})
    for nb in (mac_dev.get("neigh") or []):
        ip = str(nb.get("ip") or "")
        mac = str(nb.get("mac") or "")
        if not ip.startswith("192.168.77.") or ip in known_ips:
            continue
        if not mac or mac.startswith("ff:") or mac == "(incomplete)":
            continue
        if any(u["ip"] == ip for u in unmanaged):
            continue
        unmanaged.append({"ip": ip, "mac": mac, "dev": nb.get("dev"),
                          "note": "ARP 可见但不在纳管清单，未纳入采集"})

    # 4) 形状推断
    evidence, shape = [], "star"
    fwd_nodes = [d["id"] for d in devs if (d.get("forwarding") or {}).get("ip_forward")]
    br_nodes = [d["id"] for d in devs if (d.get("forwarding") or {}).get("bridges")]
    multi_uplink = []
    for d in devs:
        carriers = [it["name"] for it in d.get("interfaces", [])
                    if it.get("carrier") and not it.get("virtual")
                    and it.get("kind") == "ethernet"]
        if len(carriers) > 1:
            multi_uplink.append({"id": d["id"], "ports": carriers})

    if relay_links:
        shape = "star+relay"
        evidence.append(
            "检测到 " + str(len(relay_links)) + " 条三层中继链路：" +
            "；".join(f"{r['child']} 挂在 {r['parent']}.{r['parent_if']} 下"
                      for r in relay_links) +
            " → 同一个 /24 被人为拆成两段广播域，不是单一 L2 星型")
    elif fwd_nodes or br_nodes:
        shape = "hybrid"
        evidence.append(f"检测到三层转发/桥接节点：forward={fwd_nodes or '无'}, "
                        f"bridge={br_nodes or '无'} → 存在非纯二层的转发路径")
    else:
        evidence.append("全部在线节点的 net.ipv4.ip_forward=0 且无 bridge 设备 → "
                        "无三层转发，主机间通信只经过二层共享介质")

    live = [d for d in devs if d.get("online") and d.get("lab_interface")
            and any(it.get("name") == d["lab_interface"] and it.get("carrier")
                    for it in d.get("interfaces", []))]
    if len(live) >= 2:
        if relay_links:
            evidence.append(f"{len(live)} 台设备在 {SUBNET} 上各有一块载波实验口，"
                            f"但其中 {', '.join(r['child'] for r in relay_links)} 的同网段"
                            f"可达性是靠三层中继实现的 → 不据此判定为单一共享 L2")
        else:
            evidence.append(f"{len(live)} 台设备在同一 /24（{SUBNET}）上各有一块处于载波状态的"
                            f"实验网口 → 判定为共享 L2（星型接交换机）")
    elif live:
        shape = "unknown"
        evidence.append(f"仅 {len(live)} 台设备有载波实验口，不足以判定拓扑形状")
    else:
        shape = "unknown"
        evidence.append("当前没有任何设备在实验网段上有载波——链路已断开或设备未上电，"
                        "拓扑形状无法判定（图中保留最后已知位置，标为离线）")
    if multi_uplink:
        evidence.append("多载波设备：" + "；".join(
            f"{m['id']} 同时 {','.join(m['ports'])} 有载波" for m in multi_uplink))

    # 5) 交换机 / 共享介质节点
    speeds = {d["id"]: d.get("lab_speed_mbps") for d in live}
    known = [v for v in speeds.values() if v]
    sw_speed = min(known) if known else None
    switches = [{
        "id": "sw", "label": "L2 交换机（不可管理）",
        "subnet": SUBNET,
        "speed_mbps": sw_speed,
        "note": "无 SSH / SNMP / LLDP，无法读取端口映射；形状为推断值",
        "manageable": False,
    }]

    # 6) links
    #    中继链路：对端是实测（/32 路由 + 邻居 MAC），标 measured；
    #    星型链路：对端是共享介质，标 inferred。
    links = []
    for r in relay_links:
        parent = by_id.get(r["parent"]) or {}
        child = by_id.get(r["child"]) or {}
        pif = next((it for it in parent.get("interfaces", [])
                    if it["name"] == r["parent_if"]), {})
        cif = lab_iface(child) or {}
        links.append({
            "from": child.get("id"), "from_if": cif.get("name"),
            "to": parent.get("id"), "to_if": r["parent_if"],
            "layer": "L3",
            "media": cif.get("media"),
            "media_basis": cif.get("media_basis"),
            # 中继侧口的介质（例如 RV1126B 用的是 USB 网卡）——这条链路物理上
            # 由"中继口 + 子设备口"两段介质拼成，两者都要如实给出
            "to_media": pif.get("media"),
            "to_media_basis": pif.get("media_basis"),
            "speed_mbps": pif.get("speed_mbps") or cif.get("speed_mbps"),
            "mac": r["child_mac"],
            "state": pif.get("state"),
            "carrier": pif.get("carrier"),
            "relay": r["relay"],
            "peer_confidence": "measured",
            "peer_evidence": "；".join(r["evidence"]),
        })
    for d in devs:
        if d["id"] in relay_by_child:
            continue
        li = lab_iface(d)
        if not li:
            continue
        links.append({
            "from": d["id"], "from_if": li["name"],
            "to": "sw", "to_if": None,
            "layer": "L2",
            "media": li["media"],
            "media_basis": li["media_basis"],
            "speed_mbps": li["speed_mbps"],
            "mac": li["mac"],
            "state": li["state"],
            "carrier": li.get("carrier"),
            "relay": False,
            "peer_confidence": "inferred",
            "peer_evidence": "同 /24 二层可达 + 无 LLDP/CDP 数据，对端只能确认为共享介质",
        })

    # 7) 逻辑数据流（实验架构，非物理）
    flows = [
        {"from": "rv1126b", "to": "ascend", "label": "成对关键帧 → LLM 复核", "kind": "data"},
        {"from": "rk3588", "to": "ascend", "label": "事件二次确认（HTTP）", "kind": "data"},
        {"from": "mac", "to": "ascend", "label": "控制 / 推理调用", "kind": "control"},
        {"from": "rv1126b", "to": "mac", "label": "MJPEG 视频流", "kind": "control"},
    ]

    limitations = [
        "交换机不可管理（无 SSH/SNMP），无法读取「设备 → 交换机端口号」映射，"
        "星型连线的对端一律标为 inferred。",
        "未检测到 LLDP/CDP 邻居信息（设备侧未安装 lldpd），因此对端设备型号无法自动确证。",
        "邻居表只能证明「曾通信过」：离线设备不会出现在邻居表，拓扑仍保留其最后已知位置。",
    ]
    if relay_links:
        limitations.append(
            "三层中继链路的父子关系是实测的（/32 主机路由 + 该口学到的 MAC），"
            "但中继节点用 proxy ARP 代答，两端看到的对端 MAC 并非对方真实网卡 MAC，"
            "因此这条链路上不能靠 MAC 判断物理对端。")
    offline = [d["id"] for d in devs if not d.get("online")]
    if offline:
        limitations.append(f"本轮探测离线：{', '.join(offline)}——其接口数据为上一轮或空，"
                           f"已标注 stale/offline，未用默认值填充。")

    return {
        "schema": "g4-topology-v1",
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S %z"),
        "generated_ts": time.time(),
        "subnet": SUBNET,
        "shape": shape,
        "shape_evidence": evidence,
        "switches": switches,
        "nodes": devs,
        "links": links,
        "relay_links": relay_links,
        "flows": flows,
        "unmanaged": unmanaged,
        "reachability": reach,
        "limitations": limitations,
        "counts": {
            "nodes": len(devs),
            "online": sum(1 for d in devs if d.get("online")),
            "links": len(links),
            "interfaces": sum(len(d.get("interfaces", [])) for d in devs),
        },
    }


def discover():
    with ThreadPoolExecutor(max_workers=len(DEVICES)) as pool:
        devs = list(pool.map(collect_device, DEVICES))
    return build_topology(devs)


# ── CLI ──────────────────────────────────────────────────────────────────────
def summary(topo):
    L = []
    L.append(f"拓扑形状: {topo['shape']}  节点 {topo['counts']['online']}/"
             f"{topo['counts']['nodes']} 在线  链路 {topo['counts']['links']} 条")
    L.append(f"网段: {topo['subnet']}   生成: {topo['generated_at']}")
    L.append("")
    for d in topo["nodes"]:
        flag = "在线" if d.get("online") else "离线"
        L.append(f"[{d['id']}] {d['label']}  {d['ip']}  {flag}")
        if not d.get("online"):
            err = (d.get("probe") or {}).get("error")
            L.append(f"    探测失败: {err}")
        for it in d.get("interfaces", []):
            if it["name"] == d.get("lab_interface"):
                mark = "*"
            elif it.get("port_role") == "relay":
                mark = "#"
            else:
                mark = " "
            ip = ",".join(it.get("ipv4") or []) or "-"
            spd = f"{it['speed_mbps']}M" if it.get("speed_mbps") else "?"
            L.append(f"   {mark} {it['name']:<8} {it['media']:<14} {it['state']:<9} "
                     f"{spd:>6}  {it.get('mac') or '-':<18} {ip}")
            L.append(f"      依据: {it.get('media_basis')}")
            if it.get("port_role") == "relay":
                L.append(f"      #  中继口，下挂 {','.join(it.get('serves') or [])}")
        for p in d.get("idle_ports", []):
            L.append(f"     ! 闲置口 {p['name']} ({p['media']}, {p['state']}) — {p['note']}")
        ul = d.get("uplink") or {}
        if ul.get("type") == "relay":
            L.append(f"     ↳ 上联: 经 {ul['via']}.{ul['via_if']} 三层中继"
                     f"（relay={'已启用' if ul.get('relay_enabled') else '未启用'}）")
        fw = d.get("forwarding") or {}
        if fw.get("bridges") or fw.get("ip_forward"):
            L.append(f"     转发: ip_forward={fw.get('ip_forward')} bridges={fw.get('bridges')}")
    um = topo.get("unmanaged") or []
    L.append("")
    L.append("未纳管设备（ARP 可见、不在清单）: " +
             (", ".join(f"{u['ip']}({u['mac']})" for u in um) if um else "无"))
    L.append("")
    L.append("形状依据:")
    for e in topo["shape_evidence"]:
        L.append("  - " + e)
    L.append("")
    L.append("限制:")
    for e in topo["limitations"]:
        L.append("  - " + e)
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="输出完整 JSON")
    ap.add_argument("--write", action="store_true", help=f"写入 {OUT}")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    topo = discover()
    if args.write:
        OUT.parent.mkdir(parents=True, exist_ok=True)
        OUT.write_text(json.dumps(topo, ensure_ascii=False, indent=1), encoding="utf-8")
        if not args.quiet:
            print(f"written: {OUT}")
    if args.json:
        print(json.dumps(topo, ensure_ascii=False, indent=1))
    elif not args.write or not args.quiet:
        print(summary(topo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
