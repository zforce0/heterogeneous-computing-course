#!/usr/bin/env python3
"""Verify the lab network (Mac -> switch -> Ascend / RK3588 / RV1126B).

Run on the Mac:  python3 eval_platform/tools/check_lab_network.py
Exit code 0 = all core checks pass, 1 = at least one core check failed.
Read-only: never changes network settings. Prints remediation hints on failure.

2026-11-13: Mac 换为 MacBook Air M1 后 AX88179B 接口由 en9 重编号为 en5；
实验网地址 192.168.77.1/24 已按 V1.2 设为静态。昇腾盒子 .10 为非核心检查
（该节点是否接入取决于交换机的实际接线）。
"""
import re
import subprocess
import sys

ASCEND, RK, RV, MAC = "192.168.77.10", "192.168.77.21", "192.168.77.22", "192.168.77.1"
IFACE = "en5"
DASH = "http://192.168.77.1:8080/"
results = []


def sh(args, timeout=12):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout).stdout
    except Exception as exc:
        return "ERR " + str(exc)


def check(name, ok, detail, core=True):
    results.append((name, bool(ok), detail, core))
    mark = "PASS" if ok else ("FAIL" if core else "WARN")
    print("[%-4s] %-28s %s" % (mark, name, detail))


def main():
    print("=== 实验网核验 %s ===" % MAC)
    info = sh(["networksetup", "-getinfo", "AX88179B"])
    ip_ok = re.search(r"IP address:\s*192\.168\.77\.1", info) is not None
    check("%s 静态地址" % IFACE, ip_ok, "Manual 192.168.77.1/24" if ip_ok else "地址已被改动: " + info.replace("\n", " ")[:120])

    rt = sh(["netstat", "-rn", "-f", "inet"])
    defaults = [l.strip() for l in rt.splitlines() if l.strip().startswith("default")]
    lab = [l.strip() for l in rt.splitlines() if re.search(r"^192\.168\.77\s+link#", l)]
    print("      default route: " + (defaults[0] if defaults else "(none)"))
    print("      lab route    : " + (lab[0] if lab else "(missing!)"))
    check("实验网直连路由存在", bool(lab), lab[0] if lab else "192.168.77/24 路由消失")

    for host, alias in [(RK, "rk3588"), (RV, "rv1126b")]:
        r = sh(["route", "-n", "get", host])
        m = re.search(r"interface:\s*(\S+)", r)
        iface = m.group(1) if m else "?"
        check("选路 %s" % host, iface == IFACE, "interface=%s (应为 %s)" % (iface, IFACE))

        p = sh(["ping", "-c", "2", "-t", "3", host])
        # 修正历史误报：'100.0% packet loss' 也包含子串 '0.0% packet loss'。
        # 用 \b 保证前面不是数字/小数点，只认真正的 0% 丢包。
        m = re.search(r"\b0\.0% packet loss", p)
        ok = m is not None
        check("ping %s" % host, ok, "0% loss" if ok else "不通")

        s = sh(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=6", alias, "hostname"])
        ok = bool(s.strip()) and "timed out" not in s and "refused" not in s
        check("ssh %s" % alias, ok, s.strip()[:60] or "无响应")

    p = sh(["ping", "-c", "2", "-t", "3", ASCEND])
    ok = re.search(r"\b0\.0% packet loss", p) is not None
    check("ping %s (ascend)" % ASCEND, ok, "0% loss" if ok else "不通", core=False)

    d = sh(["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}", DASH])
    check("dashboard HTTP", d.strip() == "200", "HTTP " + d.strip(), core=False)

    proxy = sh(["scutil", "--proxy"])
    enabled = [l.strip() for l in proxy.splitlines() if re.search(r"(HTTPEnable|HTTPSEnable|SOCKSEnable|ProxyAutoConfigEnable)\s*:\s*1", l)]
    print("      系统代理: " + ("开启 " + ", ".join(enabled) if enabled else "全部关闭"))
    utuns = sh(["ifconfig", "-a"])
    tunnel = []
    for m in re.finditer(r"(?ms)^(utun\d+):(.*?)(?=^\w+\d*:|\Z)", utuns):
        ip = re.search(r"inet ([\d.]+)", m.group(2))
        if ip:
            tunnel.append((m.group(1), ip.group(1)))
    print("      隧道接口: " + (", ".join("%s=%s" % t for t in tunnel) if tunnel else "(无)"))

    failed = [n for n, ok, _, core in results if core and not ok]
    print("\n=== 结果: %s ===" % ("全部通过" if not failed else "失败 " + ", ".join(failed)))
    if failed:
        print("""
修复顺序:
  1) VPN 客户端打开 "Allow LAN connections" / 关闭 kill switch（最常见原因）
  2) 确认 en5 未被重置:  sudo networksetup -setmanual "AX88179B" 192.168.77.1 255.255.255.0
  3) 强制补一条直连路由: sudo route -n add -net 192.168.77.0/24 -interface en5
  4) 链路 up 但全部设备不通时，先查交换机端口/设备供电：
     bash eval_platform/tools/recheck_lab_link.sh
  5) 仍不通则先断开 VPN 复核，再逐项排查 pf 规则
""")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
