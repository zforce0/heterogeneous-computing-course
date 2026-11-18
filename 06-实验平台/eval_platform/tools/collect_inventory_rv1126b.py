#!/usr/bin/env python3
"""E00 read-only inventory collector for RV1126B (Ubuntu 22.04 / netplan).

Plan rules (section 10 / 6.1 / 3):
  - missing tool / failed command => status "N/A", never infer hardware absence
  - enumerate cameras without changing USB mode or starting a stream
  - REDACT credentials (netplan contains a Wi-Fi PSK); never write keys into the bundle
Separate file from collect_inventory.py so the RK3588 run's collector hash stays valid.
"""
import json
import os
import re
import socket
import subprocess
import sys

RUN_DIR = sys.argv[1]
TRUSTED_UTC = sys.argv[2] if len(sys.argv) > 2 else None
INV_DIR = os.path.join(RUN_DIR, "inventory")
os.makedirs(INV_DIR, exist_ok=True)

CRED = re.compile(r"(?i)\b(password|passwd|psk|wpa|secret|token|api_key)\b\s*[:=]\s*\S+")


def redact(text):
    return CRED.sub(lambda m: m.group(1) + ": [REDACTED]", text or "")


def run(cmd, timeout=12):
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        out, err, rc = redact(p.stdout.strip()), redact(p.stderr.strip()), p.returncode
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
        "hostnamectl | head -n 8",
    ],
    "cpu": ["lscpu | head -n 20", "nproc", "cat /sys/devices/system/cpu/online"],
    "memory": ["free -h", "grep MemTotal /proc/meminfo"],
    "storage": ["df -h", "lsblk -o NAME,SIZE,TYPE,MOUNTPOINT"],
    "network": [
        "ip -br addr",
        "ip -br link",
        "ip route",
        "cat /sys/class/net/eth1/speed",
        "cat /sys/class/net/eth1/operstate",
        "grep -rhE '^\\s*(eth[0-9]+:|dhcp4:|addresses:|renderer:|optional:)' /etc/netplan/ 2>/dev/null",
        "systemctl is-active systemd-networkd",
        "ethtool eth1",
    ],
    "usb": ["lsusb", "lsusb -t"],
    "camera": [
        "v4l2-ctl --list-devices",
        "ls -l /sys/class/video4linux/",
        "for d in /dev/video*; do echo \"--- $d\"; "
        "udevadm info --query=property --name=$d 2>/dev/null | "
        "grep -E '^ID_(VENDOR|MODEL|SERIAL|PATH|VID|PID|BUS)|^DEVPATH' ; done",
        "for d in /dev/video0 /dev/video1 /dev/video2 /dev/video3; do "
        "[ -e $d ] || continue; echo \"--- $d formats\"; "
        "v4l2-ctl -d $d --list-formats 2>/dev/null | head -n 20; done",
    ],
    "npu_sdk": [
        "lsmod | grep -iE 'rknpu|rknn'",
        "ls -l /usr/lib/librknnrt.so /usr/lib/librknn_api.so /usr/include/rknn_api.h",
        "ls -l /usr/bin/rknn* 2>/dev/null",
        "sha256sum /userdata/rv1126_test/yolov8m_rv1126b.rknn /userdata/dual_demo/yolov8m_rv1126b.rknn 2>/dev/null",
        "dpkg -l | grep -iE 'rknn|rockchip|opencv|v4l|ffmpeg' | head -n 30",
    ],
    "toolchain": ["gcc --version | head -n 1", "g++ --version | head -n 1",
                  "cmake --version | head -n 1", "python3 --version"],
    "services": ["systemctl is-active ssh", "grep -E '^[^#]*(PasswordAuthentication|PermitRootLogin|PubkeyAuthentication)' /etc/ssh/sshd_config"],
    "clock": ["date -u +%Y-%m-%dT%H:%M:%SZ", "timedatectl 2>/dev/null | head -n 6"],
}

inventory = {
    "schema_version": "e00-inventory-v1",
    "node_id": "rv1126b-easy-eai-nano-tb",
    "run_id": os.path.basename(RUN_DIR),
    "collection_time_utc": TRUSTED_UTC,
    "clock_source": "mac-host UTC is authoritative; board time recorded separately under clock",
    "board_hostname": socket.gethostname(),
    "credential_policy": "all command output is redacted; netplan Wi-Fi PSK never stored",
    "tool_missing_policy": "missing tool => status N/A; never infer hardware absence from a failed command",
    "sections": {},
}

raw = []
for name, cmds in SECTIONS.items():
    results = [run(c) for c in cmds]
    inventory["sections"][name] = results
    for item in results:
        raw.append("=== [%s] %s ===" % (name, item["cmd"]))
        raw.append(json.dumps(item, ensure_ascii=False, indent=2))
        raw.append("")

with open(os.path.join(INV_DIR, "inventory.json"), "w") as f:
    json.dump(inventory, f, ensure_ascii=False, indent=2)
with open(os.path.join(INV_DIR, "raw_commands.txt"), "w") as f:
    f.write("\n".join(raw))

na = sum(1 for s in inventory["sections"].values() for r in s if r.get("status") == "N/A")
total = sum(len(s) for s in inventory["sections"].values())
msg = "inventory written: %d commands, %d N/A (tool missing/unavailable)\n" % (total, na)
with open(os.path.join(RUN_DIR, "stdout.log"), "w") as f:
    f.write(msg)
with open(os.path.join(RUN_DIR, "stderr.log"), "w") as f:
    f.write("")
with open(os.path.join(RUN_DIR, "exit_status.json"), "w") as f:
    json.dump({"run_id": inventory["run_id"], "status": "completed", "exit_code": 0,
               "finished_utc": TRUSTED_UTC, "clock_source": inventory["clock_source"],
               "commands_total": total, "commands_na": na}, f, ensure_ascii=False, indent=2)
print(msg, end="")
