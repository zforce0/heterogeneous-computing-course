#!/usr/bin/env python3
"""E00 read-only inventory collector.

Rules from the plan (section 10 / appendix C):
  - tool missing / command failure => status "N/A", never infer hardware absence
  - never write private keys, credentials, or a full system tree into the result bundle
"""
import json
import os
import socket
import subprocess
import sys

RUN_DIR = sys.argv[1]
TRUSTED_UTC = sys.argv[2] if len(sys.argv) > 2 else None
INV_DIR = os.path.join(RUN_DIR, "inventory")
os.makedirs(INV_DIR, exist_ok=True)


def run(cmd, timeout=12):
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        out, err, rc = p.stdout.strip(), p.stderr.strip(), p.returncode
        if rc == 127 or "not found" in err.lower() or "command not found" in err.lower():
            return {"cmd": cmd, "status": "N/A", "reason": "tool_missing", "stderr": err}
        return {"cmd": cmd, "rc": rc, "stdout": out, "stderr": err}
    except subprocess.TimeoutExpired:
        return {"cmd": cmd, "status": "N/A", "reason": "timeout"}
    except Exception as exc:
        return {"cmd": cmd, "status": "N/A", "reason": type(exc).__name__ + ": " + str(exc)}


SECTIONS = {
    "os": [
        "uname -a",
        "cat /etc/os-release",
        "getconf LONG_BIT",
        "cat /proc/device-tree/model",
        "cat /proc/device-tree/compatible",
    ],
    "cpu": [
        "lscpu",
        "nproc",
        "grep -m1 'model name' /proc/cpuinfo",
        "cat /sys/devices/system/cpu/online",
    ],
    "memory": ["free -h", "grep MemTotal /proc/meminfo"],
    "storage": ["df -h", "lsblk -o NAME,SIZE,TYPE,MOUNTPOINT"],
    "network": [
        "ip -br addr",
        "ip route",
        "nmcli -t dev status",
        "cat /sys/class/net/eth1/speed",
        "cat /sys/class/net/eth1/operstate",
        "ethtool eth1",
    ],
    "usb": ["lsusb", "lsusb -t", "v4l2-ctl --list-devices"],
    "npu_sdk": [
        "lsmod | grep -i rknpu",
        "ls -l /usr/lib/librknnrt.so* /usr/lib/aarch64-linux-gnu/librknnrt.so* 2>/dev/null",
        "find /usr -maxdepth 4 -iname '*rknn*' -printf '%p\\n' 2>/dev/null | head -n 40",
        "dpkg -l | grep -iE 'rknn|rockchip|opencv|v4l|ffmpeg' | head -n 40",
        "cat /sys/kernel/debug/rknpu/version 2>/dev/null",
    ],
    "toolchain": [
        "gcc --version",
        "g++ --version",
        "cmake --version",
        "python3 --version",
        "getconf GNU_LIBC_VERSION",
    ],
    "services": ["systemctl is-active ssh", "systemctl --version | head -n 1"],
}

inventory = {
    "schema_version": "e00-inventory-v1",
    "node_id": "rk3588-lubancat",
    "run_id": os.path.basename(RUN_DIR),
    "collection_time_utc": TRUSTED_UTC,
    "clock_source": "mac-host UTC; board RTC is unsynced, board local time is NOT trusted",
    "board_hostname": socket.gethostname(),
    "tool_missing_policy": "missing tool => status N/A; never infer hardware absence from a failed command",
    "sections": {},
}

raw = []
for name, cmds in SECTIONS.items():
    results = []
    for cmd in cmds:
        item = run(cmd)
        results.append(item)
        raw.append("=== [%s] %s ===" % (name, cmd))
        raw.append(json.dumps(item, ensure_ascii=False, indent=2))
        raw.append("")
    inventory["sections"][name] = results

with open(os.path.join(INV_DIR, "inventory.json"), "w") as f:
    json.dump(inventory, f, ensure_ascii=False, indent=2)
with open(os.path.join(INV_DIR, "raw_commands.txt"), "w") as f:
    f.write("\n".join(raw))

na = sum(1 for s in inventory["sections"].values() for r in s if r.get("status") == "N/A")
total = sum(len(s) for s in inventory["sections"].values())
summary = "inventory written: %d commands, %d N/A (tool missing/unavailable)\n" % (total, na)
with open(os.path.join(RUN_DIR, "stdout.log"), "w") as f:
    f.write(summary)
with open(os.path.join(RUN_DIR, "stderr.log"), "w") as f:
    f.write("")
with open(os.path.join(RUN_DIR, "exit_status.json"), "w") as f:
    json.dump({
        "run_id": inventory["run_id"],
        "status": "completed",
        "exit_code": 0,
        "finished_utc": TRUSTED_UTC,
        "clock_source": inventory["clock_source"],
        "commands_total": total,
        "commands_na": na,
    }, f, ensure_ascii=False, indent=2)
print(summary, end="")
