#!/usr/bin/env python3
"""Discover whichever board is on the direct-cable interface. No root needed.

Works for both boards: IPv6 link-local multicast first (works even with no IPv4
configured), then ARP, then a bounded IPv4/port-22 sweep.
"""
import argparse
import concurrent.futures
import ipaddress
import re
import socket
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--iface", default="en9")
p.add_argument("--scan", action="store_true", help="sweep the whole /24 on iface for port 22")
p.add_argument("--net", default=None, help="override sweep network, e.g. 192.168.77.0/24")
a = p.parse_args()


def sh(cmd, timeout=10):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    except Exception:
        return ""


iface = a.iface
print("=== %s local config ===" % iface)
cfg = sh(["ifconfig", iface])
for line in cfg.splitlines():
    if any(k in line for k in ("inet ", "status:", "media:")):
        print("  " + line.strip())

local_v4 = []
for line in cfg.splitlines():
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+) netmask 0x([0-9a-f]+)", line)
    if m:
        local_v4.append((m.group(1), int(m.group(2), 16)))

print("=== IPv6 link-local neighbours ===")
sh(["ping6", "-c", "3", "-i", "1", "ff02::1%" + iface], timeout=8)
ndp = sh(["ndp", "-an"])
v6 = []
for line in ndp.splitlines():
    if iface not in line or "permanent" in line:
        continue
    m = re.match(r"(fe80::[0-9a-f:]+)%" + re.escape(iface), line.strip())
    if m:
        v6.append(m.group(1))
        print("  " + line.strip())
if not v6:
    print("  (none)")

print("=== ARP neighbours on %s ===" % iface)
arp = sh(["arp", "-a", "-n"])
arp_v4 = []
for line in arp.splitlines():
    if iface in line and "incomplete" not in line:
        m = re.search(r"\? \((\d+\.\d+\.\d+\.\d+)\)", line)
        if m:
            arp_v4.append(m.group(1))
            print("  " + line.strip())
if not arp_v4:
    print("  (none)")

cands = set(arp_v4)
cands.update(["192.168.77.22", "192.168.77.21", "192.168.3.252", "169.254.77.21"])
if a.scan:
    net = a.net
    if not net and local_v4:
        ip, mask = local_v4[0]
        bits = bin(mask).count("1")
        net = str(ipaddress.ip_network("%s/%d" % (ip, bits), strict=False))
    if net:
        print("=== sweeping %s ===" % net)
        cands.update(str(h) for h in ipaddress.ip_network(net).hosts())

print("=== probing port 22 (%d addresses) ===" % len(cands))
scope = socket.if_nametoindex(iface)


def probe(host, fam=socket.AF_INET):
    s = socket.socket(fam, socket.SOCK_STREAM)
    s.settimeout(0.8)
    try:
        if fam == socket.AF_INET6:
            s.connect((host, 22, 0, scope))
        else:
            s.connect((host, 22))
        s.settimeout(0.8)
        try:
            banner = s.recv(120).decode("utf-8", "replace").strip()
        except Exception:
            banner = ""
        return host, True, banner
    except Exception:
        return host, False, ""
    finally:
        s.close()


jobs = [(h, socket.AF_INET) for h in sorted(cands)] + [(h, socket.AF_INET6) for h in v6]
found = []
with concurrent.futures.ThreadPoolExecutor(max_workers=48) as ex:
    for host, ok, banner in ex.map(lambda t: probe(*t), jobs):
        if ok:
            fam = "v6" if ":" in host else "v4"
            print("  OPEN [%s] %s  %s" % (fam, host, banner))
            found.append((host, fam))

print("=== suggestion ===")
if not found:
    print("  No SSH endpoint. Check cable/power, then run RV local console init first.")
for host, fam in found:
    target = host + "%" + iface if fam == "v6" else host
    user = "root" if "192.168.77.22" in host or "192.168.3.252" in host else "cat"
    alias = {"192.168.77.22": "rv1126b", "192.168.3.252": "rv1126b-legacy",
             "192.168.77.21": "rk3588"}.get(host)
    print("  ssh %s@%s   %s" % (user, target, ("(or: ssh %s)" % alias) if alias else ""))
