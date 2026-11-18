#!/usr/bin/env python3
"""采集一个节点「现在在跑什么」的最小事实集，供看板汇总。

只用标准库，Mac / Ubuntu / Debian 通吃（板端没有 numpy/psutil）。
输出 JSON：主机、负载、关键进程、关键服务、模型文件。
不做任何推断：进程不在列表里就不写，服务取不到就写 unknown。
"""
import json
import os
import platform
import subprocess
import sys
import time

# 进程关键字 → 它在系统里的职责（只匹配 args 里出现的这些名字）
WATCH = [
    ("llama-server", "中心 LLM 推理（llama.cpp）"),
    ("llama-cli", "LLM 推理（llama.cpp CLI）"),
    ("ir_snap", "热成像厂商 SDK 取帧（usbfs 独占）"),
    ("yolo_live", "可见光 YOLOv8 推理（NPU）"),
    ("ir_detect", "热成像检测（NPU + 热斑）"),
    ("camera_stream.py", "双光流服务 :8082"),
    ("serve_g4.py", "看板后端 :8899"),
    ("g4_guard.py", "USB/网口守护"),
    ("rknn_server", "RKNN 运行时"),
    ("rk_llm_reconfirm.py", "端侧事件送 LLM 复核"),
    ("evidence_server.py", "G3 证据服务 :9100"),
    ("uvicorn", "HTTP 服务"),
]

# 服务名 → 用途（存在才报告状态）
SERVICES = [
    ("g4-llm", "昇腾 LLM 服务"),
    ("camera-preview", "RV 双光流"),
    ("yolo-live", "RV 可见光推理"),
    ("ir-detect", "RV 热成像检测"),
    ("g4-guard", "RV USB/网口守护"),
    ("g3-evidence", "RK G3 证据服务"),
    ("g4-dhcp", "RK DHCP 兜底"),
]

MODEL_DIRS = [
    "/userdata/rv1126_test",
    "/userdata/dual_demo",
    "/root/eval_dashboard",
    "/home/cat/eval_dashboard",
    "/home/cat/models",
]
MODEL_EXT = (".rknn", ".gguf", ".onnx", ".pt", ".bin")


def sh(cmd, timeout=6):
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return p.stdout.strip()
    except Exception:
        return ""


def procs():
    raw = sh("ps -eo pid,pcpu,etime,args")
    if not raw:
        raw = sh("ps -Ao pid,pcpu,etime,command")
    out = []
    seen = set()
    for line in raw.splitlines()[1:]:
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        pid, pcpu, etime, args = parts
        for key, label in WATCH:
            if key in args and key not in seen:
                seen.add(key)
                try:
                    cpu = float(pcpu)
                except Exception:
                    cpu = None
                out.append({"key": key, "label": label, "pid": int(pid) if pid.isdigit() else None,
                            "cpu": cpu, "etime": etime, "args": args[:180]})
                break
    return out


def services():
    out = []
    if not os.path.exists("/run/systemd/system"):
        return out
    for name, label in SERVICES:
        st = sh("systemctl is-active %s 2>/dev/null" % name, timeout=4) or "unknown"
        out.append({"name": name, "label": label, "active": st})
    return out


def models():
    out = []
    for d in MODEL_DIRS:
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            if not n.lower().endswith(MODEL_EXT):
                continue
            p = os.path.join(d, n)
            try:
                st = os.stat(p)
            except OSError:
                continue
            out.append({"path": p, "mb": round(st.st_size / 1048576.0, 1),
                        "mtime": time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))})
    out.sort(key=lambda x: -x["mb"])
    return out[:8]


def main():
    data = {
        "host": platform.node(),
        "os": ("%s %s" % (platform.system(), platform.release()))[:60],
        "ts": time.strftime("%H:%M:%S"),
        "loadavg": [round(x, 2) for x in os.getloadavg()] if hasattr(os, "getloadavg") else None,
        "procs": procs(),
        "services": services(),
        "models": models(),
    }
    json.dump(data, sys.stdout, ensure_ascii=False)


if __name__ == "__main__":
    main()
