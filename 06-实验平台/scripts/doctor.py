#!/usr/bin/env python3
"""部署自检（只读）：确认源码完整、端口可用、远端服务是否真的在线。

    python3 scripts/doctor.py

不联网修改任何设备，不打印任何凭据；实验网设备未上电时如实报离线，
而不是把归档快照当成「在线」。
"""
import json
import socket
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "eval_platform" / "config"))
import lab_config as lab  # noqa: E402

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
REQUIRED = [
    "eval_platform/dashboard/serve_g4.py",
    "eval_platform/dashboard/g4_live.template.html",
    "eval_platform/dashboard/g4_live.html",
    "eval_platform/dashboard/g4_state.json",
    "eval_platform/tools/node_roster.py",
    "eval_platform/tools/discover_topology.py",
    "eval_platform/config/lab_config.py",
    "config/lab.env.example",
    "docs/01-environment.md",
    "docs/02-deployment.md",
    "docs/03-user-guide.md",
    "docs/04-principles.md",
    "docs/05-course-map.md",
]


def mark(ok, msg, detail=""):
    tag = "[OK]  " if ok else "[WARN]"
    print("%s %s%s" % (tag, msg, ("  -- " + detail) if detail else ""))
    return bool(ok)


def probe(url, expect=200):
    try:
        with OPENER.open(url, timeout=2.5) as r:
            return r.status == expect, str(r.status)
    except Exception as exc:
        return False, type(exc).__name__


def main():
    bad = 0
    print("== 互联异构计算课程工程 · 部署自检 ==")
    print("Python: %s" % sys.version.split()[0])
    bad += not mark(sys.version_info >= (3, 8), "Python >= 3.8")
    print("配置来源: %s%s" % (lab.ENV_FILE, "（未创建，使用课程示例值）"
                              if not lab.ENV_FILE.exists() else ""))
    print()

    print("-- 源码与页面 --")
    for rel in REQUIRED:
        p = ROOT / rel
        bad += not mark(p.is_file() and p.stat().st_size > 0, rel,
                        "" if p.exists() else "缺失")
    print()

    print("-- 本机监听端口 --")
    host, port = lab.dash_bind()
    s = socket.socket()
    try:
        s.bind((host, port))
        print("[OK]   %s:%d 可绑定（控制台可启动）" % (host, port))
    except OSError as exc:
        # 已占用不一定是故障：可能控制台已经在跑
        detected = False
        for probe_path in ("/api/state", "/api/config"):
            try:
                with OPENER.open("http://%s:%d%s" % (host, port, probe_path), timeout=2) as r:
                    r.read(128)
                detected = True
                break
            except Exception:
                pass
        if detected:
            print("[OK]   %s:%d 已有本平台在监听（可用 API 探测到）" % (host, port))
        else:
            # 端口被占用但无法识别，不等于本机不可部署：可能是旧版服务或别的应用。
            print("[--]   %s:%d 已被占用且无法识别（换端口请用 DASH_PORT）" % (host, port))
    finally:
        s.close()
    print()

    print("-- 远端服务（离线不算错误，归档快照仍可展示）--")
    checks = [
        ("中心 LLM", lab.endpoint("ascend", "llm") + "/health"),
        ("RV1126B 视觉", lab.endpoint("rv1126b", "vision") + "/status.json"),
        ("RK3588 板端", lab.endpoint("rk3588", "dash") + "/status.json"),
    ]
    for name, url in checks:
        ok, detail = probe(url)
        print("%s %-14s %s  [%s]" % ("[OK]  " if ok else "[--]  ", name, url, detail))
    print()

    print("-- SSH 别名（只读检查配置，不发起连接）--")
    ssh_cfg = Path.home() / ".ssh" / "config"
    if ssh_cfg.exists():
        text = ssh_cfg.read_text(encoding="utf-8", errors="replace")
        for role in ("ascend", "rk3588", "rv1126b"):
            alias = lab.ssh(role)
            ok = alias and ("Host %s" % alias) in text
            print("%s %-10s alias=%s" % ("[OK]  " if ok else "[--]  ", role, alias))
    else:
        print("[--]   ~/.ssh/config 不存在；节点实时名单将只显示本机")
    print()

    if bad:
        print("结论：有 %d 项需要修复（见上面的 WARN）。" % bad)
        return 1
    print("结论：本机可部署；远端服务状态以实际探测为准。")
    print("下一步：make page && make run，然后打开 http://%s:%d/" % (host, port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
