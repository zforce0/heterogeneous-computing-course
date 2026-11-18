#!/usr/bin/env python3
"""Discover the three lab boards on the experiment subnet (read-only).

Topology as of 2026-11-14 00:00: STAR topology. Mac .1 / RK3588 .21 /
RV1126B .22 / Ascend .10 all attach to the lab switch on 192.168.77.0/24.
The switch is NOT uplinked to the office LAN. The office subnet scan is opt-in
(--with-office) and produces many incomplete ARP entries, so it is off by
default.

Known issue at 00:00: RK3588 still has the old chain-topology rk-forward.service
proxy ARP enabled, so .21/.22/.23 all resolve to RK eth1's MAC and RV1126B is
unreachable. See disable_rk_proxy_arp.sh; this script only reports what it sees.

Fingerprints (archived V1.1 manual / G3 report):
  RK3588  : 8081 dashboard, 9100 evidence-server, SSH user 'cat', Debian 11
  RV1126B : 8081 dashboard, 8082 camera-preview, SSH user 'root', Ubuntu 22.04
  Ascend  : SSH user 'HwHiAiUser'/'root', port 22, possibly a MindIE service

Beware: 9100 is ALSO the standard printer RAW/JetDirect port. A 9100 hit alone
is NOT evidence of the G3 protocol -- cross-check SSH banner / HTTP title.
"""
import concurrent.futures
import re
import socket
import subprocess
import sys
import urllib.request

LAB_SUBNET = "192.168.77."
OFFICE_SUBNET = "192.168.1."
PORTS = [22, 80, 8080, 8081, 8082, 9100, 11434]


def ping(ip, timeout=2):
    r = subprocess.run(["ping", "-c", "1", "-t", str(timeout), ip],
                       capture_output=True, text=True)
    o = r.stdout
    return "0.0% packet loss" in o and "100.0% packet loss" not in o


def tcp(ip, port, timeout=0.9):
    try:
        s = socket.create_connection((ip, port), timeout=timeout)
        s.settimeout(timeout)
        banner = ""
        if port in (22, 23):
            try:
                banner = s.recv(120).decode("utf8", "ignore").strip()
            except Exception:
                pass
        s.close()
        return port, banner
    except Exception:
        return None


def http_title(ip, port, timeout=1.5):
    try:
        with urllib.request.urlopen(f"http://{ip}:{port}/", timeout=timeout) as r:
            body = r.read(3000).decode("utf8", "ignore")
            server = r.headers.get("Server", "")
            m = re.search(r"<title[^>]*>(.*?)</title>", body, re.I | re.S)
            return (m.group(1).strip()[:70] if m else ""), server[:70]
    except Exception:
        return "", ""


def classify(open_ports, banners, titles):
    reasons = []
    score = {"rk3588": 0, "rv1126b": 0, "ascend": 0}
    if 8081 in open_ports:
        score["rk3588"] += 2
        score["rv1126b"] += 2
        reasons.append("8081 实验 dashboard")
    if 8082 in open_ports:
        score["rv1126b"] += 3
        reasons.append("8082 相机预览(RV)")
    if 9100 in open_ports:
        score["rk3588"] += 1
        reasons.append("9100(也可能是打印机 RAW，需复核)")
    b = " ".join(banners.values())
    if "Debian" in b:
        score["rk3588"] += 2
        reasons.append("SSH banner Debian(RK)")
    if "Ubuntu" in b:
        score["rv1126b"] += 1
        reasons.append("SSH banner Ubuntu(也可能是普通 Ubuntu 主机)")
    t = " ".join(titles.values())
    if "Benchmark" in t or "分布式多核异构" in t:
        score["rk3588"] += 3
        score["rv1126b"] += 3
        reasons.append("实验 dashboard 标题命中")
    if "电网大亨" in t:
        score["rk3588"] -= 3
        score["rv1126b"] -= 3
        reasons.append("网页为「电网大亨」应用，非实验 dashboard")
    if "Inkjet" in t or "HP HTTP" in t or "Brother" in t:
        score["rk3588"] -= 5
        score["rv1126b"] -= 5
        reasons.append("确认为打印机")
    best = max(score, key=score.get)
    if score[best] < 3:
        return "unknown(办公室设备或未识别)", reasons
    return best, reasons


def en5_state():
    out = subprocess.run(["ifconfig", "en5"], capture_output=True, text=True).stdout
    return ("status: active" in out,
            re.search(r"inet\s+([\d.]+)", out).group(1)
            if re.search(r"inet\s+([\d.]+)", out) else None)


def main():
    with_office = "--with-office" in sys.argv
    print("=== 互联异构实验设备发现 (只读, 00:00 星型拓扑) ===")
    active, addr = en5_state()
    print(f"en5 链路: {'active' if active else 'inactive'}  地址: {addr or '无'}")
    print("星型拓扑：Mac/RK3588/RV1126B/昇腾盒子同挂交换机，192.168.77.0/24。")
    print()

    if not active:
        print("!! en5 物理层未 up，实验网 192.168.77.0/24 根本不可达，跳过全段扫描。")
        print("   这是供电/网线问题，不是 IP 配置问题（配置仍在：Manual 192.168.77.1/24）。")
        print("   先检查：交换机电源指示灯、网线两端、三台板子是否上电。")
        print("   恢复后再跑：bash eval_platform/tools/recheck_lab_link.sh")
        if not with_office:
            return 2
        print("   (--with-office 已指定，仅继续做办公室网对照扫描)")

    subnets = [("lab", LAB_SUBNET)] if active else []
    if with_office:
        subnets.append(("office", OFFICE_SUBNET))

    candidates = []
    for label, prefix in subnets:
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
            alive = [f"{prefix}{i}" for i, ok in
                     ex.map(lambda i: (i, ping(f"{prefix}{i}")), range(1, 255)) if ok]
        print(f"{label} {prefix}0/24 活跃主机 {len(alive)} 台: "
              + (" ".join(alive) if alive else "(无)"))
        for ip in alive:
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
                res = [r for r in ex.map(lambda p: tcp(ip, p), PORTS) if r]
            open_ports = [r[0] for r in res]
            banners = {r[0]: r[1] for r in res if r[1]}
            titles = {}
            for p in (80, 8080, 8081):
                if p in open_ports:
                    t, srv = http_title(ip, p)
                    titles[p] = (t + " " + srv).strip()
            guess, reasons = classify(open_ports, banners, titles)
            candidates.append((ip, open_ports, banners, titles, guess, reasons))

    if candidates:
        print("\n=== 主机画像 ===")
        for ip, ports, banners, titles, guess, reasons in candidates:
            desc = " ".join(f"{p}:{v}" for p, v in titles.items() if v)
            ssh = " ".join(banners.values())[:70]
            print(f"{ip:<16} ports={ports} guess={guess}")
            if ssh:
                print(f"    ssh: {ssh}")
            if desc:
                print(f"    http: {desc[:150]}")
            if reasons:
                print(f"    依据: {'; '.join(reasons)}")

    hits = [c for c in candidates if c[4] in ("rk3588", "rv1126b", "ascend")]
    print("\n=== 结论 ===")
    if not active:
        print("链路未恢复，无法发现任何实验设备（未伪造扫描结果）。")
    elif not hits:
        print("链路已 up 但未发现可确认的实验设备；三台板子可能仍在上电或地址不同。")
    else:
        for ip, ports, _, _, guess, reasons in hits:
            print(f"候选 {guess}: {ip} ports={ports} 依据={'; '.join(reasons)}")
        print("下一步：ssh 过去核对 hostname / uname -a，再跑 probe_ascend_box.sh。")
    return 0 if hits else (2 if not active else 1)


if __name__ == "__main__":
    sys.exit(main())
