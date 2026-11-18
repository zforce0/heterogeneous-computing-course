#!/usr/bin/env python3
"""G4 实时 dashboard 服务 v2（Mac 仅显示/控制，不跑推理）。

    python3 eval_platform/dashboard/serve_g4.py --port 8899

接口:
  GET  /                          -> g4_live.html
  GET  /api/state                 -> 构建期状态快照（节点/关键帧/口径）
  GET  /api/status                -> 四节点一次性探测（兼容旧页面）
  GET  /api/stream                -> SSE：每 2s 推送节点/LLM/视觉指标 + 事件
  GET  /api/series                -> 近 6 分钟时间序列（曲线用）
  GET  /api/metrics               -> 昇腾 llama-server Prometheus 指标（已解析）
  GET  /stream/vis.mjpg           -> RV1126B 可见光 MJPEG 实时流
  GET  /stream/ir.mjpg            -> RV1126B 热成像 MJPEG 实时流
  GET  /frame/vis.jpg             -> 单帧可见光（截图/降级用）
  GET  /frame/ir.png              -> 单帧热成像
  GET  /api/snapshot?kind=vis     -> 把当前帧存进 samples/，返回文件名
  GET  /api/control?action=start  -> 代理 RV1126B 视觉服务 start/pause/stop
  POST /api/llm                   -> 一次性推理（兼容旧页面）
  POST /api/llm/stream            -> 流式推理（SSE，逐 token 回传）
  POST /api/bench                 -> 3 条标准用例实测吞吐

只绑定 127.0.0.1；不修改网络设置，不伪造任何实测数据。
"""
import argparse
import base64
import concurrent.futures
import functools
import http.server
import json
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib
import urllib.error
import urllib.request
from collections import deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
SAMPLES = HERE / "samples"
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

# 实验室参数（config/lab.env，缺省为课程示例值）与拓扑自动发现
sys.path.insert(0, str(HERE.parent / "config"))
sys.path.insert(0, str(HERE.parent / "tools"))
try:
    import lab_config as lab
except Exception:  # noqa: BLE001 —— 缺配置模块时退回原内置值
    lab = None
try:
    import discover_topology as topo_mod
except Exception as _exc:  # noqa: BLE001
    topo_mod = None
    _TOPO_IMPORT_ERR = str(_exc)

RV = lab.endpoint("rv1126b", "vision") if lab else "http://192.168.77.22:8082"
RK = lab.endpoint("rk3588", "dash") if lab else "http://192.168.77.21:8081"
LLM = lab.endpoint("ascend", "llm") if lab else "http://192.168.77.10:8080"

HOSTS = [
    {"id": "mac", "ip": lab.ip("mac") if lab else "192.168.77.1", "port": None},
    {"id": "ascend", "ip": lab.ip("ascend") if lab else "192.168.77.10", "port": 8080},
    {"id": "rk3588", "ip": lab.ip("rk3588") if lab else "192.168.77.21", "port": 9100},
    {"id": "rv1126b", "ip": lab.ip("rv1126b") if lab else "192.168.77.22", "port": 8082},
]

TICK = 2.0
HIST = 180  # 6 分钟 @2s

# ─── 共享状态 ────────────────────────────────────────────────────────────────
LOCK = threading.Lock()
FRAME_COND = threading.Condition(LOCK)
SEQ = 0
FRAMES = {"vis": (b"", 0.0), "ir": (b"", 0.0)}
EVENTS = deque(maxlen=300)
SNAP = {
    "t": 0.0,
    "checked_at": "--:--:--",
    "nodes": [],
    "llm": {"online": False, "metrics": {}, "tok_s": None, "tokens_total": None},
    "vision": {},
    "rk": {},
}
SERIES = {
    "t": deque(maxlen=HIST),
    "vis_fps": deque(maxlen=HIST),
    "tok_s": deque(maxlen=HIST),
    "rtt": {h["id"]: deque(maxlen=HIST) for h in HOSTS},
}

# ─── 节点分工名单（每 25s 通过 SSH 采一次，页面按缓存渲染）───────────────────
# 角色是项目约定的固定分工；「在跑什么」全部实测（进程/服务/模型文件），
# 不写死任何进程名以外的推断，采不到就报 unavailable。
ROLES = {
    "mac": {"role": "显示 / 控制", "detail": "浏览器 + 本服务聚合；不承担任何推理"},
    "ascend": {"role": "中心 LLM", "detail": "llama.cpp OpenAI 兼容 API :8080"},
    "rk3588": {"role": "端侧视觉 / 事件复核", "detail": "G3 证据服务 :9100 + 板端 dashboard :8081"},
    "rv1126b": {"role": "双光采集 / 端侧检测", "detail": "可见光 YOLOv8 + 热成像 NPU/热斑，:8082"},
}
ROSTER_NODES = [("mac", None),
                ("ascend", lab.ssh("ascend") if lab else "ascend"),
                ("rk3588", lab.ssh("rk3588") if lab else "rk3588"),
                ("rv1126b", lab.ssh("rv1126b") if lab else "rv1126b")]
ROSTER_TTL = 25.0
ROSTER_SCRIPT = HERE.parent / "tools/node_roster.py"
ROSTER = {"ts": 0.0, "nodes": {}, "error": None}
ROSTER_LOCK = threading.Lock()
# llama.cpp 的 /props 里有真实模型文件路径（/v1/models 只有别名），30s 取一次
LLM_MODEL = {"path": None, "alias": None, "n_ctx": None, "ts": 0.0}

# ─── 拓扑缓存（SSH 采集较慢，60s 一轮，页面只读缓存）────────────────────────
TOPO = {"data": None, "ts": 0.0, "error": None, "refreshing": False}
TOPO_TTL = 60.0
TOPO_LOCK = threading.Lock()


def refresh_topology(force=False):
    """采集一轮拓扑。force=False 时若未过期则直接返回缓存。"""
    with TOPO_LOCK:
        fresh = TOPO["data"] and (time.time() - TOPO["ts"] < TOPO_TTL)
        if fresh and not force:
            return TOPO["data"]
        if TOPO["refreshing"]:
            return TOPO["data"]
        TOPO["refreshing"] = True
    try:
        if topo_mod is None:
            raise RuntimeError("discover_topology 导入失败: %s" % _TOPO_IMPORT_ERR)
        data = topo_mod.discover()
        with TOPO_LOCK:
            TOPO["data"] = data
            TOPO["ts"] = time.time()
            TOPO["error"] = None
        return data
    except Exception as exc:  # noqa: BLE001
        with TOPO_LOCK:
            TOPO["error"] = str(exc)[:300]
        return TOPO["data"]
    finally:
        with TOPO_LOCK:
            TOPO["refreshing"] = False


def topology_worker():
    """后台每 30s 检查一次，过期就重扫（首次启动立刻扫）。"""
    while True:
        try:
            refresh_topology()
        except Exception:
            pass
        time.sleep(30)


# ─── 节点分工：谁在做什么、跑什么模型 ─────────────────────────────────────────
def _roster_ssh(host):
    """把 node_roster.py 推到远端 /tmp 再执行（远端无需预装任何东西）。"""
    src = base64.b64encode(ROSTER_SCRIPT.read_text(encoding="utf-8").encode()).decode()
    p = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host,
         "base64 -d > /tmp/node_roster.py && python3 /tmp/node_roster.py"],
        input=src.encode(), capture_output=True, timeout=25)
    if p.returncode != 0 or not p.stdout.strip():
        raise RuntimeError((p.stderr.decode("utf-8", "replace") or "ssh 无输出")[:140])
    return json.loads(p.stdout.decode("utf-8", "replace"))


def _roster_local():
    """本机（Mac）。沙箱/权限不允许枚举进程时如实说明，不假装有数据。"""
    p = subprocess.run([sys.executable, str(ROSTER_SCRIPT)], capture_output=True, timeout=15)
    if not p.stdout.strip():
        raise RuntimeError((p.stderr.decode("utf-8", "replace") or "采集脚本无输出")[:140])
    d = json.loads(p.stdout.decode("utf-8", "replace"))
    if not d.get("procs"):
        d["note"] = "进程列表不可读（macOS 权限/沙箱限制）"
    return d


def refresh_roster(force=False):
    if not force and time.time() - ROSTER["ts"] < ROSTER_TTL and ROSTER["nodes"]:
        return ROSTER
    nodes, errs = {}, {}
    for nid, host in ROSTER_NODES:
        try:
            nodes[nid] = _roster_ssh(host) if host else _roster_local()
        except Exception as exc:  # noqa: BLE001
            errs[nid] = str(exc)[:140]
            nodes[nid] = {"unavailable": str(exc)[:140]}
    with ROSTER_LOCK:
        ROSTER.update({"ts": time.time(), "nodes": nodes, "error": errs or None})
    return ROSTER


def roster_worker():
    while True:
        # 中心 LLM 上真正加载的模型文件（/props 的 model_path），与进程名单一起刷新
        try:
            p = json.loads(fetch(LLM + "/props", 4).decode("utf-8", "replace"))
            LLM_MODEL.update({"path": p.get("model_path"), "alias": p.get("model_alias"),
                              "n_ctx": (p.get("default_generation_settings") or {}).get("n_ctx"),
                              "ts": time.time()})
        except Exception:
            pass
        try:
            refresh_roster(force=True)
        except Exception:
            pass
        time.sleep(ROSTER_TTL)


def roster_view():
    """给页面用的合并视图：固定角色 + 实测进程/服务/模型 + 当前实时指标。"""
    with ROSTER_LOCK:
        snap = {"ts": ROSTER["ts"], "nodes": dict(ROSTER["nodes"]), "error": ROSTER["error"]}
    with LOCK:
        vis = (SNAP.get("vision") or {}).copy()
        llm = (SNAP.get("llm") or {}).copy()
        nodes_probe = {n["id"]: n for n in (SNAP.get("nodes") or [])}
    out = {}
    for nid, _host in ROSTER_NODES:
        base = snap["nodes"].get(nid) or {}
        role = ROLES.get(nid, {})
        item = {
            "id": nid,
            "role": role.get("role"),
            "detail": role.get("detail"),
            "online": (nodes_probe.get(nid) or {}).get("online"),
            "rtt_ms": (nodes_probe.get(nid) or {}).get("rtt_ms"),
            "loadavg": base.get("loadavg"),
            "procs": base.get("procs") or [],
            "services": [s for s in (base.get("services") or []) if s.get("active") == "active"],
            "models": base.get("models") or [],
            "note": base.get("note"),
            "error": base.get("unavailable"),
        }
        # Mac 上 ps 常被权限挡住：本进程是确定在跑的，补上事实，其余如实说明
        if nid == "mac" and not item["procs"]:
            item["procs"] = [{"key": "serve_g4.py", "label": "看板后端 :8899（本进程）", "cpu": None}]
            item["note"] = item.get("note") or "其他进程不可枚举（macOS 权限限制）"
        # 端侧实际加载的模型（比目录扫描更准：这是"正在跑"的那个）
        if nid == "rv1126b":
            names = []
            for m in (vis.get("vis_model"), vis.get("ir_model")):
                if m and m.get("name") and m["name"] not in names:
                    names.append(m["name"])
            item["running_models"] = names
            item["metrics"] = {"vis_fps": vis.get("vis_fps"), "ir_fps": vis.get("ir_fps"),
                               "vis_infer_ms": vis.get("infer_ms"),
                               "ir_infer_ms": (vis.get("ir_det") or {}).get("infer_ms")}
        elif nid == "ascend":
            item["running_models"] = [llm.get("model_path")] if llm.get("model_path") else []
            item["metrics"] = {"tok_s": llm.get("tok_s"),
                               "tokens_total": llm.get("tokens_total")}
        elif nid == "rk3588":
            rk = (SNAP.get("rk") or {})
            item["running_models"] = [(m.get("path") or "").split("/")[-1]
                                      for m in item["models"]][:2]
            item["metrics"] = {"detections": len(rk.get("detections") or [])}
        else:
            item["running_models"] = []
            item["metrics"] = {"checked_at": (SNAP.get("checked_at") or "")}
        out[nid] = item
    return {"ts": snap["ts"], "age_s": round(time.time() - snap["ts"], 1) if snap["ts"] else None,
            "nodes": out, "error": snap["error"]}


def roster_text():
    """压成短文本给 LLM 当实时上下文（0.6B 读长文会跑偏，控制在十行内）。"""
    rv = roster_view()
    lines = ["系统分工（实时实测，%s 采集）：" % (
        ("%.0fs 前" % rv["age_s"]) if rv.get("age_s") is not None else "尚未采集")]
    for nid, n in rv["nodes"].items():
        if n.get("error"):
            lines.append("- %s：%s（探测失败）" % (nid, n.get("role")))
            continue
        procs = "、".join(p["key"] for p in (n.get("procs") or [])[:4]) or "无关键进程"
        models = "、".join([m for m in (n.get("running_models") or []) if m]) or "无模型"
        m = n.get("metrics") or {}
        mets = []
        if m.get("vis_fps") is not None:
            mets.append("可见光 %.1ffps" % m["vis_fps"])
        if m.get("ir_fps") is not None:
            mets.append("热成像 %.1ffps" % m["ir_fps"])
        if m.get("tok_s") is not None:
            mets.append("LLM %.1f tok/s" % m["tok_s"])
        lines.append("- %s（%s）：%s；进程 %s；模型 %s%s" % (
            nid, n.get("role"), n.get("detail") or "", procs, models,
            ("；" + "，".join(mets)) if mets else ""))
    return "\n".join(lines)


def build_evidence(seconds=60.0):
    """把窗口内的真实采样聚合成一份可核对的事实块。

    返回 (data, error)。所有数字都来自端侧上报，不做任何外推；
    缺什么就留空/不出现，交给 LLM 说"数据不足"。
    """
    now = time.time()
    with WIN_LOCK:
        recs = [r for r in WIN if now - r["t"] <= seconds]
    if len(recs) < 3:
        return None, "窗口内采样不足（服务刚启动？至少等 6 秒再分析）"

    t0, t1 = recs[0]["t"], recs[-1]["t"]
    span = max(0.5, t1 - t0)
    n = len(recs)

    def eff_fps(key):
        a, b = recs[0].get(key), recs[-1].get(key)
        if a is None or b is None or b < a:
            return None
        return round((b - a) / span, 1)

    def avg(key):
        vals = [r.get(key) for r in recs if r.get(key) is not None]
        return round(sum(vals) / len(vals), 1) if vals else None

    cls = {}
    for r in recs:
        seen = set()
        for d in r["dets"]:
            name = d["name"] or "?"
            c = cls.setdefault(name, {"hit": 0, "scores": [], "area": 0})
            c["scores"].append(d["score"] or 0)
            c["area"] = max(c["area"], d["area"] or 0)
            if name not in seen:
                c["hit"] += 1
                seen.add(name)
    classes = [{"name": k, "hit": c["hit"], "of": n,
                "max": round(max(c["scores"]), 2),
                "mean": round(sum(c["scores"]) / len(c["scores"]), 2),
                "max_area": c["area"]}
               for k, c in sorted(cls.items(), key=lambda kv: -kv[1]["hit"])]

    irs = [r["ir_lum"] for r in recs if r.get("ir_lum")]
    ir_lum = None
    if irs:
        means = [x["mean"] for x in irs]
        contrast = [x["p98"] - x["p2"] for x in irs]
        ir_lum = {"n": len(irs), "size": "%dx%d" % (irs[0]["w"], irs[0]["h"]),
                  "mean_lo": round(min(means), 1), "mean_hi": round(max(means), 1),
                  "mean_avg": round(sum(means) / len(means), 1),
                  "contrast_avg": round(sum(contrast) / len(contrast), 1)}

    with LOCK:
        fails = [e for e in EVENTS
                 if now - e.get("t", 0) <= seconds and e["level"] in ("warn", "bad")]

    # 热成像端侧检测聚合：NPU 目标按类别计数，热斑只看数量与面积/峰值（相对灰度）
    ird_list = [r["ir_det"] for r in recs if r.get("ir_det")]
    ir_det = None
    if ird_list:
        n_ir = len(ird_list)
        cls2 = {}
        for r in ird_list:
            seen2 = set()
            for d in r["yolo"]:
                name = d["name"] or "?"
                c = cls2.setdefault(name, {"hit": 0, "scores": [], "area": 0})
                c["scores"].append(d["score"] or 0)
                c["area"] = max(c["area"], d["area"] or 0)
                if name not in seen2:
                    c["hit"] += 1
                    seen2.add(name)
        classes2 = [{"name": k, "hit": c["hit"], "of": n_ir,
                     "max": round(max(c["scores"]), 2),
                     "mean": round(sum(c["scores"]) / len(c["scores"]), 2),
                     "max_area": c["area"]}
                    for k, c in sorted(cls2.items(), key=lambda kv: -kv[1]["hit"])]
        ms2 = [r["infer_ms"] for r in ird_list if r.get("infer_ms")]
        res2 = next((r["res"] for r in ird_list if r.get("res")), None)
        ir_det = {
            "n": n_ir,
            "classes": classes2,
            "blob_n_avg": round(sum(r["blob_n"] for r in ird_list) / n_ir, 1),
            "blob_area_max": max(r["blob_area"] for r in ird_list),
            "blob_peak_max": max(r["blob_peak"] for r in ird_list),
            "infer_ms": round(sum(ms2) / len(ms2), 1) if ms2 else None,
            "res": ("%dx%d" % (res2[0], res2[1])) if isinstance(res2, (list, tuple)) and len(res2) == 2 else None,
            "engine": (ird_list[-1].get("engine") or None),
        }

    res = next((r["res"] for r in recs if r.get("res")), None)
    data = {
        "seconds": round(span, 1),
        "samples": n,
        "vis": {
            "ready_ratio": round(sum(1 for r in recs if r["vis_ready"]) / n, 2),
            "fps_avg": avg("vis_fps"), "fps_eff": eff_fps("vis_frame"),
            "res": ("%dx%d" % (res[0], res[1])) if isinstance(res, (list, tuple)) and len(res) == 2 else None,
            "infer_ms": avg("infer_ms"),
            "port": recs[-1].get("vis_port"),
            "classes": classes,
        },
        "ir": {
            "ready_ratio": round(sum(1 for r in recs if r["ir_ready"]) / n, 2),
            "fps_avg": avg("ir_fps"), "fps_eff": eff_fps("ir_frame"),
            "port": recs[-1].get("ir_port"),
            "lum": ir_lum,
            "det": ir_det,
        },
        "abnormal": [{"t": e["ts"], "msg": e["msg"]} for e in fails[:5]],
        "abnormal_n": len(fails),
        "models": {
            "vis": sorted({r.get("vis_model") for r in recs if r.get("vis_model")}),
            "ir": sorted({r.get("ir_model") for r in recs if r.get("ir_model")}),
        },
    }
    return data, None


def evidence_text(ev):
    """把事实块压成紧凑文本——端侧 LLM 只有 ~11 tok/s，证据必须短。"""
    v, i = ev["vis"], ev["ir"]
    px = None
    if v["res"]:
        try:
            w, h = [int(x) for x in v["res"].lower().split("x")]
            px = w * h
        except Exception:
            px = None
    lines = ["窗口 %.0fs，%d 次采样（每 2s 一次）" % (ev["seconds"], ev["samples"]),
             "可见光：就绪 %d%%，%.1ffps（端侧 %.1ffps），口 %s，%s" % (
                 round(v["ready_ratio"] * 100), v["fps_avg"] or 0,
                 v["fps_eff"] or 0, v["port"] or "未探测", v["res"] or "分辨率未知")]
    M = ev.get("models") or {}
    if M.get("vis") or M.get("ir"):
        lines.append("在图模型：可见光 %s；热成像 %s" % (
            "、".join(M.get("vis") or []) or "未上报",
            "、".join(M.get("ir") or []) or "未上报"))
    if v["classes"]:
        lines.append("检测（次数/%d，最高分，均分，最大框占画面）：" % ev["samples"])
        for c in v["classes"][:5]:
            share = ("%.1f%%" % (100.0 * c["max_area"] / px)) if px and c["max_area"] else "N/A"
            lines.append("  %s %d/%d max%.2f avg%.2f %s" % (
                c["name"], c["hit"], c["of"], c["max"], c["mean"], share))
    else:
        lines.append("检测：窗口内 YOLO 未报出任何目标")
    lines.append("热成像：就绪 %d%%，%.1ffps（端侧 %.1ffps），口 %s" % (
        round(i["ready_ratio"] * 100), i["fps_avg"] or 0,
        i["fps_eff"] or 0, i["port"] or "未探测"))
    L = i["lum"]
    if L:
        lines.append("  相对灰度 0-255（非温度）：均值 %.1f，波动 %.1f~%.1f，对比度 %.1f，%d 帧 %s" % (
            L["mean_avg"], L["mean_lo"], L["mean_hi"], L["contrast_avg"], L["n"], L["size"]))
    else:
        lines.append("  无亮度统计（热成像未出图）")
    D = i.get("det")
    if D:
        px_ir = None
        if D["res"]:
            try:
                w2, h2 = [int(x) for x in D["res"].lower().split("x")]
                px_ir = w2 * h2
            except Exception:
                px_ir = None
        if D["classes"]:
            lines.append("  端侧检测（NPU %s）：%s" % (
                D["res"] or "?", "；".join("%s %d/%d max%.2f" % (
                    c["name"], c["hit"], c["of"], c["max"]) for c in D["classes"][:4])))
        else:
            lines.append("  端侧检测（NPU %s）：窗口内未报出目标" % (D["res"] or "?"))
        share = ("%.1f%%" % (100.0 * D["blob_area_max"] / px_ir)) if px_ir and D["blob_area_max"] else "N/A"
        lines.append("  热斑（传统 CV，相对灰度）：平均 %.1f 个/帧，最大 %d px（%s 画面），峰值 %d" % (
            D["blob_n_avg"], D["blob_area_max"], share, D["blob_peak_max"]))
    else:
        lines.append("  端侧检测：无结果（ir_detect 未运行或产物过期）")
    if ev["abnormal"]:
        lines.append("异常 %d 条：" % ev["abnormal_n"] +
                     "；".join("%s %s" % (e["t"], e["msg"]) for e in ev["abnormal"][:3]))
    else:
        lines.append("异常：窗口内无掉线/取帧失败")
    return "\n".join(lines)


def q_seconds(q, default=60.0):
    """从提问里读时间窗口（「近 5 分钟」→ 300s），夹在 20~300s。"""
    m = re.search(r"(\d+)\s*分钟", q or "")
    if not m:
        return default
    return max(20.0, min(300.0, float(m.group(1)) * 60.0))


def compare_line(ev):
    """跨模态量化对比：两路各取同名类别，由后端算分差与命中率差。

    热成像端侧是复用可见光的 YOLOv8m 权重（跨模态），同一目标分数天然偏低，
    「低多少、命中率差几个点」正是本实验要量的东西，不能让 0.6B 自己去猜或算。
    这里只算可核对的量，不下「正常/异常」的结论——那是 LLM 那三行的事。

    刻意写短：0.6B 很容易把这一整行原样搬进「理由」，太长就会挤掉后面的内容
    （实测 160 token 被截断在「确保多源数据的融合与」）。细节留在证据块里给页面悬停看。
    """
    v, i = ev["vis"], ev["ir"]
    D = i.get("det") or {}
    vc = v["classes"] or []
    ic = {c["name"]: c for c in (D.get("classes") or [])}
    parts = ["可见光 %.1ffps/就绪%d%%、热成像 %.1ffps/就绪%d%%" % (
        v["fps_avg"] or 0, round(v["ready_ratio"] * 100),
        i["fps_avg"] or 0, round(i["ready_ratio"] * 100))]
    gap = hr_gap = None
    if vc:
        c = vc[0]
        s = "可见光 %s %d/%d max%.2f" % (c["name"], c["hit"], c["of"], c["max"])
        o = ic.get(c["name"])
        if o:
            gap = round(c["max"] - o["max"], 2)
            hr_v = round(100.0 * c["hit"] / max(1, c["of"]))
            hr_i = round(100.0 * o["hit"] / max(1, o["of"]))
            hr_gap = abs(hr_v - hr_i)
            s += " ↔ 热成像 %s %d/%d max%.2f（分差 %.2f，命中率 %d%%/%d%%，%s）" % (
                o["name"], o["hit"], o["of"], o["max"], gap, hr_v, hr_i,
                "一致" if (hr_gap <= 20 and gap <= 0.30) else "偏弱")
        else:
            # 跨模态漏检是这套复用权重的固有现象，但必须给出可引用的数字，
            # 否则 0.6B 会自己编一个「分差」（实测编出「分差 0.6 个/帧」）。
            others = D.get("classes") or []
            if others:
                t = others[0]
                s += " ↔ 热成像未报出 %s（跨模态漏检；热成像最强 %s %d/%d max%.2f）" % (
                    c["name"], t["name"], t["hit"], t["of"], t["max"])
            else:
                s += " ↔ 热成像本窗一个目标都没报出（可见光 %s %d/%d，跨模态漏检）" % (
                    c["name"], c["hit"], c["of"])
        parts.append(s)
    parts.append("热斑 %.1f 个/帧" % (D.get("blob_n_avg") or 0))
    parts.append("异常 %d 条" % ev["abnormal_n"])
    return "；".join(parts)


def verdict(ev):
    """机械判定：只看就绪率、异常条数、跨模态是否整窗漏检，不让 0.6B 猜。

    依据都是系统可观测的量（有没有掉线、取帧失败、热成像有没有报出同名类），
    不含任何语义推断；「这个人该不该关注」不在端侧模型的判断范围内，也不该由它编。
    """
    if ev["abnormal_n"] > 0:
        return "异常"
    v, i = ev["vis"], ev["ir"]
    if v["ready_ratio"] < 0.9 or i["ready_ratio"] < 0.9:
        return "关注"
    # 可见光稳稳有目标、热成像整窗没报出同名类 = 跨模态复用权重漏检，
    # 这是要观察的现象，标「关注」而不是「正常」，也不算故障。
    vc = v["classes"] or []
    if vc and vc[0]["hit"] >= 0.8 * max(1, vc[0]["of"]):
        if vc[0]["name"] not in {c["name"] for c in ((i.get("det") or {}).get("classes") or [])}:
            return "关注"
    return "正常"


def tidy_verdict(text):
    """研判输出格式规整：0.6B 常把「建议：」挤进理由那一行。

    只补换行、不改任何文字，避免把三行结论糊成一段。
    """
    out = []
    for line in (text or "").strip().splitlines():
        line = line.strip()
        if not line:
            continue
        for k in ("理由：", "建议："):
            i = line.find(k)
            if i > 0:
                head = line[:i].strip(" 　;,，。、")
                if head:
                    out.append(head)
                line = line[i:]
        # 0.6B 有时把「建议」两字写进内容里：「建议：建议进一步验证」→ 去掉重复的那个
        line = re.sub(r"^(建议：)\s*建议", r"\1", line)
        out.append(line)
    return "\n".join(out)


_PLACEHOLDER_LINE = re.compile(r"^(理由|建议)：\s*(一句话|补一句.*)$")


def has_placeholder(text):
    """研判输出里是否还留着模板占位。

    0.6B 大约有三分之一概率把「理由：一句话」原样吐回来（temperature=0.2 也会抖动），
    这不是提示词能根治的，只能靠重试兜底，或者如实标注。
    """
    for line in (text or "").splitlines():
        if _PLACEHOLDER_LINE.match(line.strip()):
            return True
    return False


def enforce_verdict(text, ev):
    """判断行一律用后端成品替换，模型写什么都不算。

    这行是 verdict() 按就绪率、异常条数、跨模态是否漏检算出来的，本来就该由
    后端负责；模型自己写时会省掉括号里的两路分数（实测「判断：关注」），
    而那两个数字正是研判要给的量化依据，不能丢。
    模型在别处把「关注」改成「异常」的情况也一并被这里挡住。
    """
    lines = [l for l in (text or "").splitlines()]
    if lines and lines[0].strip().startswith("判断："):
        lines[0] = verdict_line(ev)
    else:
        lines.insert(0, verdict_line(ev))
    return "\n".join(lines).strip()


def strip_placeholders(text):
    """把没被替换的占位行整行删掉——宁可少一行，也不能把「一句话」当结论。"""
    return "\n".join(l for l in (text or "").splitlines()
                      if not _PLACEHOLDER_LINE.match(l.strip())).strip()


def verdict_line(ev):
    """把判断行做成成品：0.6B 只需照抄，不必自己选词（实测它选错过）。"""
    v, i = ev["vis"], ev["ir"]
    vt = (v["classes"] or [{}])[0]
    D = i.get("det") or {}
    it = {c["name"]: c for c in (D.get("classes") or [])}.get(vt.get("name"))
    a = ("可见光 max%.2f" % vt["max"]) if vt.get("max") is not None else "可见光无目标"
    b = ("热成像 max%.2f" % it["max"]) if it else "热成像未报出同类"
    return "判断：%s（%s / %s）" % (verdict(ev), a, b)


def analysis_rule(ev):
    """研判口径的输出契约。

    聊天框（/api/llm/stream 的 analysis 分流）和「近 1 分钟双光分析」chip
    （/api/analyze/stream）共用这一份，避免两条入口的说法漂移。

    实测 0.6B 的坑（试了 5 版才定下来）：
      · 模板行只要带可执行文字（「从正常/关注/异常里选…」「，并用对比里的数字说明理由」
        「（补一句，不超过 30 字）」），它都会整段抄进答案，一字不改；
      · 只有「理由：一句话」「建议：一句话」这种三字占位它才会真正替换；
      · 给了具体判据句反而更糟，所以判据写在后端 verdict() 里，判断行直接给成品。
    所以：判断行 = 后端成品（它只可能抄对），理由/建议 = 极短占位。
    """
    return (
        "上面这次双光观测正常还是异常？只输出三行，不要标题：\n"
        + verdict_line(ev) + "\n"
        "理由：一句话\n"
        "建议：一句话\n"
        "不要逐条复述事件；热成像没有温度，不要写摄氏度。"
    )


def analysis_prompt(ev):
    """研判口径（≠ 事件列举）：不列事件，只给判断 + 理由 + 建议。

    和「近一分钟有哪些事件」的分工：
      事件列举 → 只有条目台账，禁止判断和建议；
      双光研判 → 只有结论三行，禁止逐条复述事件。
    0.6B 的两个实测坑：给数值示例会照抄示例、只给标题会原样吐回标题。
    事实只放 compare_line 这一份——重复出现的数字，它会整段搬进「理由」。
    """
    return (
        "对比：" + compare_line(ev) + "\n\n"
        "上面是同一个 %d 秒窗口里可见光与热成像的实测数据。\n" % round(ev["seconds"])
        + analysis_rule(ev)
    )


def bump():
    global SEQ
    with LOCK:
        SEQ += 1
        FRAME_COND.notify_all()


def push_event(level, msg):
    with LOCK:
        EVENTS.appendleft({"ts": time.strftime("%H:%M:%S"), "t": time.time(),
                           "level": level, "msg": msg})


# ─── 监测事件：把「采样」变成「变化」────────────────────────────────────────
# 事件只记状态切换（目标出现/离开、热斑起落、帧率骤降/恢复、亮度突变），
# 稳态（现在有哪些目标、多少 fps）另由 recent_events 的 steady 给出。
# 这样问「近一分钟有哪些事件」时不会得到 30 条「chair 在场」的复述。
MON_LOCK = threading.Lock()
MON = {"cls": {}, "blob": None, "vis_fps": None, "lum": None}
EVT_GAP = 8.0      # 连续这么久没检出才算离开（YOLO 有抖动，不能一帧没检到就报）
LUM_JUMP = 15.0    # 灰度均值跳变阈值，单位是 0-255 相对灰度，不是温度
EVT_MIN_SCORE = 0.40   # 事件门限：低于此分的弱检测（常是 0.27~0.35 的 mouse/book 抖动）
                       # 只体现在「当前稳态」，不单独发事件，否则一分钟能被它刷满


def monitor_events(now, dets, ird, vis_fps, lum):
    """对比上一次采样，只在发生切换时产生一条事件；没有切换就什么都不做。"""
    out = []                                  # 先在锁外收集，再统一推送
    with MON_LOCK:
        seen = set()
        for d in dets:
            name = (d.get("name") or "").strip()
            if not name:
                continue
            if (d.get("score") or 0) < EVT_MIN_SCORE:
                continue                      # 弱检测不发事件，但会进「当前稳态」
            seen.add(name)
            m = MON["cls"].get(name)
            if m is None:                     # 首次观测：静默建立基线，不刷「检测到」
                MON["cls"][name] = {"last": now, "on": True, "hits": 1}
                continue
            gap = now - m["last"]
            m["last"] = now
            # 连续命中计数：低分弱目标会一帧有一帧无，必须连续 2 次（约 4s）
            # 才认定为「真的出现」，否则一分钟里会刷出十几条 mouse 进出。
            # 隔了 3s 以上（等于漏掉一个采样）就重新计数。
            m["hits"] = 1 if gap > 3.0 else m.get("hits", 0) + 1
            if not m["on"] and m["hits"] >= 2:
                m["on"] = True
                out.append(("evt", "可见光检测到 %s（分数 %.2f）" % (name, d.get("score") or 0)))
        for name, m in MON["cls"].items():
            if name in seen or not m["on"]:
                continue
            if now - m["last"] >= EVT_GAP:    # 连续未检出足够久，才算真的离开
                m["on"] = False
                out.append(("evt", "%s 离开可见光视野（%.0f 秒未检出）" % (name, now - m["last"])))

        if ird is not None:
            n = int(ird.get("blob_n") or 0)
            prev = MON["blob"]
            if prev is not None:
                if prev == 0 and n >= 2:
                    out.append(("evt", "热成像出现 %d 个热斑（峰值灰度 %d）" % (
                        n, ird.get("blob_peak") or 0)))
                elif prev >= 2 and n == 0:
                    out.append(("evt", "热成像热斑消失（此前 %d 个）" % prev))
            MON["blob"] = n

        if vis_fps is not None:
            prev = MON["vis_fps"]
            if prev is not None:
                if prev >= 4.0 and vis_fps < 2.0:
                    out.append(("warn", "可见光帧率跌到 %.1f fps" % vis_fps))
                elif prev < 2.0 and vis_fps >= 4.0:
                    out.append(("ok", "可见光帧率恢复 %.1f fps" % vis_fps))
            MON["vis_fps"] = vis_fps

        if lum and lum.get("mean") is not None:
            prev, mv = MON["lum"], lum["mean"]
            if prev is not None and abs(mv - prev) >= LUM_JUMP:
                out.append(("evt", "热成像灰度均值 %.0f → %.0f（%+.0f，相对灰度非温度）" % (
                    prev, mv, mv - prev)))
            MON["lum"] = mv

    for level, msg in out:                    # 锁外推送，避免 MON_LOCK 与 LOCK 嵌套
        push_event(level, msg)


def recent_events(seconds=60.0):
    """近 N 秒的监测事件 + 当前稳态，全部来自实测采样。"""
    now = time.time()
    with LOCK:
        raw = [dict(e) for e in EVENTS if now - e.get("t", 0) <= seconds]
    raw.sort(key=lambda e: e["t"])
    alarms = [e for e in raw if e["level"] in ("warn", "bad")]
    changes = [e for e in raw if e["level"] == "evt"]
    recovers = [e for e in raw if e["level"] == "ok"]
    ops_n = sum(1 for e in raw if e["level"] == "info")

    with WIN_LOCK:
        recs = [r for r in WIN if now - r["t"] <= seconds]
    steady = {}
    if recs:
        n = len(recs)
        hit, mx = {}, {}
        for r in recs:
            per = {}
            for d in r["dets"]:
                k = (d.get("name") or "?").strip()
                per[k] = max(per.get(k, 0) or 0, d.get("score") or 0)
            for k, s in per.items():
                hit[k] = hit.get(k, 0) + 1
                mx[k] = max(mx.get(k, 0), s)
        blob = [r["ir_det"]["blob_n"] for r in recs if r.get("ir_det")]
        steady = {
            "samples": n,
            "vis_ready": round(sum(1 for r in recs if r["vis_ready"]) / n, 2),
            "ir_ready": round(sum(1 for r in recs if r["ir_ready"]) / n, 2),
            "vis_fps": recs[-1].get("vis_fps"),
            "ir_fps": recs[-1].get("ir_fps"),
            "blob_n_avg": round(sum(blob) / len(blob), 1) if blob else None,
            "classes": [{"name": k, "ratio": round(hit[k] / n, 2), "max": round(mx[k], 2)}
                        for k in sorted(hit, key=lambda k: -hit[k])],
        }
    return {
        "seconds": seconds,
        "now": time.strftime("%H:%M:%S", time.localtime(now)),
        "since": time.strftime("%H:%M:%S", time.localtime(now - seconds)),
        "alarms": alarms[-6:], "changes": changes[-10:], "recoveries": recovers[-4:],
        "counts": {"alarm": len(alarms), "change": len(changes), "recover": len(recovers)},
        "ops_n": ops_n,
        "steady": steady,
    }


EVT_TAG = {"warn": "告警", "bad": "故障", "ok": "恢复", "evt": "变化"}


def events_text(seconds=60.0, limit=6, meta=None):
    """压成给 LLM 的短文本：事件按时间排序 + 一行稳态。meta 回填条目数。"""
    r = recent_events(seconds)
    items = sorted(r["alarms"] + r["changes"] + r["recoveries"], key=lambda e: e["t"])
    lines = ["近 %.0f 秒监测事件（%s ~ %s，本地实测）：" % (r["seconds"], r["since"], r["now"])]
    shown = items[-limit:]
    if meta is not None:
        meta["n"] = len(shown)
    if not items:
        lines.append("- 无状态变化事件：没有目标进出、没有告警、没有掉线")
    else:
        for i, e in enumerate(shown, 1):
            tag = EVT_TAG.get(e["level"], "")
            lines.append("%d) %s %s%s" % (i, e["ts"], ("[" + tag + "] ") if tag else "", e["msg"]))
        if len(items) > limit:
            lines.append("（另有 %d 条同类事件未列出）" % (len(items) - limit))
    st = r.get("steady") or {}
    if st:
        cls = "、".join("%s 在场%d%%(max%.2f)" % (c["name"], round(c["ratio"] * 100), c["max"])
                        for c in st["classes"][:4]) or "无目标"
        lines.append("当前稳态（不是事件）：可见光 %.1ffps 就绪%d%%，%s；热成像 %.1ffps 就绪%d%%，热斑 %.1f 个/帧" % (
            st.get("vis_fps") or 0, round(st.get("vis_ready", 0) * 100), cls,
            st.get("ir_fps") or 0, round(st.get("ir_ready", 0) * 100), st.get("blob_n_avg") or 0))
    if r["ops_n"]:
        lines.append("（另有 %d 条系统操作记录，如启动/存证/LLM 调用，不计入监测事件）" % r["ops_n"])
    return "\n".join(lines)


# ─── 近 1 分钟窗口：给「双光事件分析」提供真实采样（不推断、不补数）─────────
WIN = deque(maxlen=240)          # 每 TICK 一条，最多覆盖 8 分钟，取多少由请求给
WIN_LOCK = threading.Lock()


def png_lum_stats(data):
    """解析端侧 256×192 RGB PNG 的亮度分布。

    只用于同一路流内部的相对比较（对比度/冷热变化），
    **不折算任何绝对温度**——模组输出的是相对灰度，没有温标。
    """
    if not data or not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    try:
        w = h = 0
        idat = []
        i = 8
        while i + 8 <= len(data):
            ln = struct.unpack(">I", data[i:i + 4])[0]
            typ = data[i + 4:i + 8]
            if typ == b"IHDR":
                w, h = struct.unpack(">II", data[i + 8:i + 16])
                depth, ctype = data[i + 16], data[i + 17]
                if depth != 8 or ctype != 2:
                    return None
            elif typ == b"IDAT":
                idat.append(data[i + 8:i + 8 + ln])
            elif typ == b"IEND":
                break
            i += 12 + ln
        if not w or not idat:
            return None
        raw = zlib.decompress(b"".join(idat))
        stride = w * 3 + 1
        rows = []
        for r in range(0, h, 2):
            row = raw[r * stride + 1:(r + 1) * stride]
            rows.append(row[0::12])          # 每 4 个像素取 1，统计足够
        flat = b"".join(rows)
        if not flat:
            return None
        s = sorted(flat)
        n = len(s)
        return {"w": w, "h": h, "mean": round(sum(flat) / n, 1),
                "p2": s[int(n * 0.02)], "p98": s[min(n - 1, int(n * 0.98))]}
    except Exception:
        return None


# ─── HTTP 工具 ───────────────────────────────────────────────────────────────
def fetch(url, timeout=3.0, data=None, headers=None):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    with OPENER.open(req, timeout=timeout) as r:
        return r.read()


def fetch_json(url, timeout=3.0):
    try:
        return json.loads(fetch(url, timeout).decode("utf-8"))
    except Exception:
        return None


def ping_rtt(ip):
    try:
        r = subprocess.run(["ping", "-c", "1", "-t", "2", ip],
                           capture_output=True, text=True, timeout=4)
        out = r.stdout or ""
        if "100.0% packet loss" in out or "round-trip" not in out:
            return None
        m = re.search(r"=\s*[\d.]+/([\d.]+)/", out)
        if m:
            return round(float(m.group(1)), 2)
        m = re.search(r"time=([\d.]+)\s*ms", out)
        return round(float(m.group(1)), 2) if m else 0.0
    except Exception:
        return None


def tcp_ok(ip, port, timeout=0.9):
    if port is None:
        return True
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except Exception:
        return False


def probe(host):
    rtt = ping_rtt(host["ip"])
    port_open = tcp_ok(host["ip"], host["port"])
    return {
        "id": host["id"], "ip": host["ip"], "port": host["port"],
        "rtt_ms": rtt, "ping": rtt is not None,
        "port_open": port_open, "online": bool(rtt is not None or port_open),
    }


def parse_prom(text):
    out = {}
    for line in (text or "").splitlines():
        if not line or line.startswith("#"):
            continue
        m = re.match(r"([\w:]+)(?:\{[^}]*\})?\s+([-\d.eE+]+)", line)
        if m:
            try:
                out[m.group(1)] = float(m.group(2))
            except ValueError:
                pass
    return out


# ─── 帧抓取线程 ──────────────────────────────────────────────────────────────
def frame_grabber():
    """以 ~8 fps 从 RV1126B 拉可见光，2 fps 拉热成像，缓存最新帧供 MJPEG 复用。"""
    last_ir = 0.0
    while True:
        try:
            data = fetch(RV + "/vis.jpg", timeout=2.0)
            if data and data.startswith(b"\xff\xd8"):
                with LOCK:
                    FRAMES["vis"] = (data, time.time())
                    FRAME_COND.notify_all()
        except Exception:
            pass
        now = time.time()
        if now - last_ir > 0.5:
            last_ir = now
            try:
                data = fetch(RV + "/ir.png", timeout=2.0)
                if data and data.startswith(b"\x89PNG"):
                    with LOCK:
                        FRAMES["ir"] = (data, time.time())
                        FRAME_COND.notify_all()
            except Exception:
                pass
        time.sleep(0.10)  # ≈8–9 fps，避免给 4 核板端和浏览器过高压力


# ─── 采集线程 ────────────────────────────────────────────────────────────────
def collector():
    prev_online = {}
    prev_tokens = None
    prev_t = None
    prev_ir = None
    while True:
        t0 = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
            nodes = list(ex.map(probe, HOSTS))
            fut_rv = ex.submit(fetch_json, RV + "/status.json", 2.5)
            fut_rk = ex.submit(fetch_json, RK + "/status.json", 2.5)
            fut_met = ex.submit(lambda: fetch(LLM + "/metrics", 2.5).decode("utf-8", "ignore"))
            rv = fut_rv.result() or {}
            rk = fut_rk.result() or {}
            try:
                met = parse_prom(fut_met.result())
            except Exception:
                met = {}

        now = time.time()
        tok_total = met.get("llamacpp:tokens_predicted_total")
        tok_s = met.get("llamacpp:predicted_tokens_seconds")
        if tok_s is None and tok_total is not None and prev_tokens is not None and prev_t:
            dt = now - prev_t
            if dt > 0.2:
                tok_s = round((tok_total - prev_tokens) / dt, 2)
        if tok_total is not None:
            prev_tokens, prev_t = tok_total, now

        llm_online = bool(met) or tcp_ok("192.168.77.10", 8080)

        for n in nodes:
            was = prev_online.get(n["id"])
            if was is not None and was != n["online"]:
                push_event("ok" if n["online"] else "bad",
                           "%s %s" % (n["id"], "恢复在线" if n["online"] else "掉线"))
            prev_online[n["id"]] = n["online"]
        ir_ready = rv.get("ir_ready")
        if prev_ir is not None and prev_ir != ir_ready:
            if ir_ready:
                _irl = rv.get("ir_link") or {}
                push_event("ok", "热成像就绪（%s fps%s）" % (
                    rv.get("ir_fps") or 0,
                    "，USB 口 " + _irl["port"] if _irl.get("port") else ""))
            else:
                why = ("USB 线缆未连接（%s）" % (rv.get("ir_vid_pid") or "0bda:5840")
                       if rv.get("ir_state") == "no_device"
                       else (rv.get("ir_error") or "掉线"))
                push_event("warn", "热成像不可用：%s" % why)
        prev_ir = ir_ready

        # 板端 g4-guard 探测到的「视频流接在哪个 USB 口」——设备编号会随插口变化，
        # 所以这里每次都取最新解析结果，前端不做任何推断。
        _cam_state = rv.get("cams") if isinstance(rv.get("cams"), dict) else {}
        _cams = _cam_state.get("cams") or {}
        # 板端 stream 服务自己从 USB 层测到的位置，用来补 g4-guard 的缺项
        _ir_direct = rv.get("ir_link") if isinstance(rv.get("ir_link"), dict) else {}

        def _link(kind):
            c = dict(_cams.get(kind) or {})
            # 热成像走厂商 SDK 时被 usbfs 独占，没有 /dev/video 节点；
            # 若守护结果还没刷新到，用 stream 服务的实测值补齐，别让端口显示空。
            if kind == "ir" and not c.get("port") and _ir_direct.get("present"):
                for f in ("port", "speed", "busnum", "devnum",
                          "product", "controller", "mode"):
                    if not c.get(f) and _ir_direct.get(f):
                        c[f] = _ir_direct[f]
                if not c.get("state"):
                    c["state"] = _ir_direct.get("mode")
                c["bus_enum"] = c.get("bus_enum") or True
                c["usb"] = c.get("usb") or True
            return {
                "port": c.get("port"),
                "speed": c.get("speed"),
                "controller": c.get("controller"),
                "devnum": c.get("devnum"),
                "node": c.get("node"),
                "alias": c.get("alias"),
                "product": c.get("product"),
                "card": c.get("card"),
                "usb": c.get("usb"),
                "bus_enum": c.get("bus_enum"),
                "mode": c.get("mode"),
                "state": c.get("state"),
            }
        cam_link = {"vis": _link("vis"), "ir": _link("ir"),
                    "ts": _cam_state.get("ts")}

        # 热成像亮度分布：取 frame_grabber 缓存的当前帧，只做相对比较（非温度）
        with LOCK:
            _ir_img, _ir_ts = FRAMES["ir"]
        ir_lum = png_lum_stats(_ir_img) if _ir_img and now - _ir_ts < 6 else None

        dets = []
        for d in (rv.get("detections") or [])[:12]:
            b = d.get("box") or [0, 0, 0, 0]
            try:
                area = max(0, int(b[2]) - int(b[0])) * max(0, int(b[3]) - int(b[1]))
            except Exception:
                area = 0
            dets.append({"name": d.get("name"), "score": d.get("score"), "area": area})

        # 热成像端侧检测结果（RV1126B 上的 ir_detect：NPU YOLOv8 + 传统 CV 热斑）
        _ird = rv.get("ir_det") if isinstance(rv.get("ir_det"), dict) else None
        ird = None
        if _ird:
            iy = []
            for d in (_ird.get("yolo") or [])[:8]:
                b = d.get("box") or [0, 0, 0, 0]
                try:
                    a = max(0, int(b[2]) - int(b[0])) * max(0, int(b[3]) - int(b[1]))
                except Exception:
                    a = 0
                iy.append({"name": (d.get("name") or "").strip(),
                           "score": d.get("score"), "area": a})
            ib = _ird.get("blobs") or []
            ird = {
                "yolo": iy,
                "blob_n": len(ib),
                "blob_area": max([b.get("area") or 0 for b in ib] or [0]),
                "blob_peak": max([b.get("peak") or 0 for b in ib] or [0]),
                "infer_ms": _ird.get("infer_ms"),
                "res": _ird.get("res"),
                "engine": _ird.get("engine"),
                "age_s": _ird.get("age_s"),
            }

        # 采样 → 事件：只有状态切换才会进事件流（供「近一分钟有哪些事件」直接回答）
        try:
            monitor_events(now, dets, ird, rv.get("vis_fps"), ir_lum)
        except Exception:
            pass

        with WIN_LOCK:
            WIN.append({
                "t": now,
                "vis_fps": rv.get("vis_fps"), "ir_fps": rv.get("ir_fps"),
                "vis_frame": rv.get("vis_frame"), "ir_frame": rv.get("ir_frame"),
                "vis_ready": bool(rv.get("vis_ready")), "ir_ready": bool(rv.get("ir_ready")),
                "infer_ms": rv.get("infer_ms"), "dets": dets,
                "res": rv.get("vis_res"),
                "ir_lum": ir_lum,
                "ir_det": ird,
                "vis_port": cam_link["vis"].get("port"),
                "ir_port": cam_link["ir"].get("port"),
                # 窗口内两路各自用的模型（换模型的实验必须能在证据里留下痕迹）
                "vis_model": (rv.get("vis_model") or {}).get("name"),
                "ir_model": (rv.get("ir_model") or {}).get("name"),
            })

        # 节点分工（缓存的 SSH 采集结果 + 当前实时指标），供页面与 LLM 共用同一份
        try:
            rview = roster_view()
        except Exception:
            rview = {"ts": 0, "nodes": {}, "error": "roster 组装失败"}

        with LOCK:
            SNAP.update({
                "t": now,
                "checked_at": time.strftime("%H:%M:%S"),
                "nodes": nodes,
                "roster": rview,
                "llm": {
                    "online": llm_online,
                    "tok_s": round(tok_s, 2) if isinstance(tok_s, (int, float)) else None,
                    "tokens_total": int(tok_total) if tok_total is not None else None,
                    "prompt_tokens_total": int(met.get("llamacpp:prompt_tokens_total", 0)) if met else None,
                    "kv_cache_usage_ratio": met.get("llamacpp:kv_cache_usage_ratio"),
                    # 盒上真正加载的 GGUF（来自 llama.cpp /props，30s 刷新一次）
                    "model_path": LLM_MODEL.get("path"),
                    "model_alias": LLM_MODEL.get("alias"),
                    "n_ctx": LLM_MODEL.get("n_ctx"),
                    "metrics": met,
                },
                "vision": {
                    "state": rv.get("state"),
                    "vis_fps": rv.get("vis_fps"),
                    "ir_fps": rv.get("ir_fps"),
                    "vis_frame": rv.get("vis_frame"),
                    "ir_frame": rv.get("ir_frame"),
                    "vis_ready": rv.get("vis_ready"),
                    "ir_ready": rv.get("ir_ready"),
                    # 可见光未就绪的原因：stale=没有新帧（设备掉线 / yolo_live 在等设备）
                    "vis_state": rv.get("vis_state"),
                    "vis_error": rv.get("vis_error"),
                    # 热成像未就绪的具体原因：no_device=USB 未枚举 / sdk_error=取帧失败
                    "ir_state": rv.get("ir_state"),
                    "ir_usb": rv.get("ir_usb"),
                    "ir_vid_pid": rv.get("ir_vid_pid"),
                    "ir_error": rv.get("ir_error"),
                    "sdk_running": rv.get("sdk_running"),
                    "uptime_s": rv.get("uptime_s"),
                    "infer_ms": rv.get("infer_ms"),
                    "detections": (rv.get("detections") or [])[:8],
                    # 两路各自实际加载的模型（端侧上报文件名/路径/字节数，换模型重启即变）
                    "vis_model": rv.get("vis_model"),
                    "ir_model": rv.get("ir_model"),
                    # 热成像端侧检测：NPU 目标框 + CV 热斑，全部在 RV1126B 上算完再上报
                    "ir_det": {
                        "state": rv.get("ir_det_state"),
                        "engine": rv.get("ir_edge_engine"),
                        "infer_ms": rv.get("ir_infer_ms"),
                        "res": rv.get("ir_det_res"),
                        "age_s": (_ird or {}).get("age_s"),
                        "yolo": (rv.get("ir_detections") or [])[:6],
                        "blobs": (rv.get("ir_blobs") or [])[:5],
                    },
                    "last_error": rv.get("last_error"),
                    "link": cam_link,
                },
                "rk": {
                    "state": rk.get("state"),
                    "vis_fps": rk.get("vis_fps"),
                    "detections": (rk.get("detections") or [])[:5],
                },
            })
            SERIES["t"].append(int(now))
            SERIES["vis_fps"].append(rv.get("vis_fps"))
            SERIES["tok_s"].append(SNAP["llm"]["tok_s"])
            for n in nodes:
                SERIES["rtt"][n["id"]].append(n["rtt_ms"])
            SEQ_INC = True
        if SEQ_INC:
            bump()

        time.sleep(max(0.2, TICK - (time.monotonic() - t0)))


# ─── HTTP 处理 ───────────────────────────────────────────────────────────────
class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def end_headers(self):
        if not getattr(self, "_stream", False):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ── GET ──
    def _send_live_html(self, body=True):
        try:
            data = (HERE / "g4_live.html").read_bytes()
        except OSError as exc:
            return self._json({"error": "g4_live.html 读取失败: %s" % exc}, 500)
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store, must-revalidate")
        self.end_headers()
        if body:
            self.wfile.write(data)

    def do_HEAD(self):
        # 目录默认会命中同目录的 index.html（另一个旧页面），这里统一到 g4_live.html
        if self.path.split("?", 1)[0] in ("/", "/g4_live.html"):
            return self._send_live_html(body=False)
        return super().do_HEAD()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""

        if path in ("/", "/g4_live.html"):
            # 自己发，不交给 SimpleHTTPRequestHandler：它只给 Last-Modified，
            # 浏览器会启发式缓存，改完页面很可能刷不出来（排查时极易误判成代码没生效）。
            return self._send_live_html()
        if path == "/api/state":
            try:
                return self._json(json.loads((HERE / "g4_state.json").read_text("utf-8")))
            except Exception as exc:
                return self._json({"error": str(exc)}, 500)
        if path == "/api/config":
            # 只回传地址/端口/ SSH 别名，不含任何凭据；用于换机器后的自检
            return self._json({"rv": RV, "rk": RK, "llm": LLM,
                               "hosts": HOSTS, "ssh": {r: lab.ssh(r) for r in ("ascend", "rk3588", "rv1126b")} if lab else {},
                               "from_file": str(lab.ENV_FILE) if lab else None})
        if path == "/api/topology":
            force = "refresh=1" in query
            if force:
                data = refresh_topology(force=True)
            else:
                with TOPO_LOCK:
                    data = TOPO["data"]
                if data is None:
                    data = refresh_topology()
            with TOPO_LOCK:
                ts, err = TOPO["ts"], TOPO["error"]
            if data is None:
                return self._json({"error": err or "拓扑尚未采集完成"}, 503)
            return self._json({**data, "cached_age_s": round(time.time() - ts, 1),
                               "last_error": err})
        if path == "/api/roster":
            # 页面/脚本都可以拿这份「谁在跑什么」；refresh=1 触发一次 SSH 重采
            if "refresh=1" in query:
                try:
                    refresh_roster(force=True)
                except Exception:
                    pass
            return self._json(roster_view())
        if path == "/api/events":
            # 近 N 秒监测事件（状态变化 + 告警 + 稳态），与喂给 LLM 的是同一份事实
            try:
                secs = float(dict(p.split("=", 1) for p in query.split("&") if "=" in p)
                             .get("seconds", 60))
            except Exception:
                secs = 60.0
            secs = max(10.0, min(600.0, secs))
            return self._json({**recent_events(secs), "text": events_text(secs)})
        if path == "/api/status":
            with LOCK:
                return self._json({"checked_at": SNAP["checked_at"], "nodes": SNAP["nodes"],
                                   "llm_api_reachable": SNAP["llm"]["online"]})
        if path == "/api/stream":
            return self._sse()
        if path == "/api/series":
            with LOCK:
                data = {"t": list(SERIES["t"]), "vis_fps": list(SERIES["vis_fps"]),
                        "tok_s": list(SERIES["tok_s"]),
                        "rtt": {k: list(v) for k, v in SERIES["rtt"].items()}}
            return self._json(data)
        if path == "/api/metrics":
            with LOCK:
                return self._json(SNAP["llm"])
        if path == "/stream/vis.mjpg":
            return self._mjpeg("vis")
        if path == "/stream/ir.mjpg":
            return self._mjpeg("ir")
        if path in ("/frame/vis.jpg", "/frame/ir.png"):
            kind = "vis" if "vis" in path else "ir"
            with LOCK:
                data, _ = FRAMES[kind]
            if not data:
                return self._json({"error": "frame not ready"}, 503)
            ctype = "image/jpeg" if kind == "vis" else "image/png"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return self.wfile.write(data)
        if path == "/api/snapshot":
            kind = "ir" if "kind=ir" in query else "vis"
            with LOCK:
                data, ts = FRAMES[kind]
            if not data:
                return self._json({"ok": False, "error": "frame not ready"}, 503)
            ext = "png" if kind == "ir" else "jpg"
            name = "live_%s_%s.%s" % (time.strftime("%Y%m%d_%H%M%S"), kind, ext)
            (SAMPLES / name).write_bytes(data)
            push_event("ok", "已存证 %s" % name)
            bump()
            return self._json({"ok": True, "file": name, "bytes": len(data)})
        if path == "/api/control":
            action = ""
            for kv in query.split("&"):
                if kv.startswith("action="):
                    action = kv.split("=", 1)[1]
            if action not in ("start", "pause", "stop"):
                return self._json({"ok": False, "error": "bad action"}, 400)
            try:
                res = fetch_json("%s/control?action=%s" % (RV, action), 6.0)
                push_event("info", "视觉服务 → %s" % action)
                bump()
                return self._json({"ok": True, "result": res})
            except Exception as exc:
                return self._json({"ok": False, "error": str(exc)[:200]}, 502)
        return super().do_GET()

    # ── POST ──
    def do_POST(self):
        path = self.path.split("?", 1)[0]
        try:
            n = int(self.headers.get("Content-Length", "0"))
            req = json.loads(self.rfile.read(n).decode("utf-8")) if n else {}
        except Exception:
            req = {}

        if path == "/api/analyze/stream":
            return self._analyze(req)
        if path == "/api/llm/stream":
            return self._llm_stream(req)
        if path == "/api/llm":
            return self._llm_once(req)
        if path == "/api/bench":
            return self._bench(req)
        return self._json({"ok": False, "error": "unknown endpoint"}, 404)

    # ── SSE 状态流 ──
    def _sse(self):
        self._stream = True
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        last_seq = -1
        last_beat = time.time()
        try:
            while True:
                with LOCK:
                    FRAME_COND.wait_for(lambda: SEQ != last_seq, timeout=15)
                    last_seq = SEQ
                    payload = {"type": "status", "seq": SEQ, **SNAP,
                               "topo_ts": TOPO["ts"],
                               "events": list(EVENTS)[:40]}
                self.wfile.write(b"data: " + json.dumps(payload, ensure_ascii=False).encode() + b"\n\n")
                self.wfile.flush()
                if time.time() - last_beat > 10:
                    last_beat = time.time()
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
        except Exception:
            return

    # ── MJPEG ──
    def _lens_reason(self, kind):
        """流不可用时给前端的可读原因（不猜，只报后端已知状态）。"""
        with LOCK:
            v = (SNAP.get("vision") or {}).copy()
        if kind == "vis":
            return "可见光流未就绪（RV1126B:8082 无新帧）"
        st = v.get("ir_state") or ""
        if st == "no_device" or v.get("ir_usb") is False:
            return "热成像模组未枚举（USB 总线无 %s）" % (v.get("ir_vid_pid") or "0bda:5840")
        if st == "sdk_error":
            return "热成像 SDK 取帧失败：%s" % (v.get("ir_error") or "未知原因")
        if v.get("state") not in ("running", None):
            return "端侧视觉服务状态：%s" % v.get("state")
        return "热成像暂无新帧（%s）" % (v.get("ir_error") or "原因未知")

    def _mjpeg(self, kind):
        # 先等一小会儿：可用就立刻出帧，不可用就快速返回 503，
        # 避免浏览器 <img> 挂在一个永不来帧的连接上（表现为"一直在转圈"）。
        deadline = time.time() + (3.0 if kind == "vis" else 0.8)
        with LOCK:
            while time.time() < deadline:
                data, ts = FRAMES[kind]
                if data and time.time() - ts < 4.0:
                    break
                FRAME_COND.wait(0.15)
            data, ts = FRAMES[kind]
        if not data or time.time() - ts > 4.0:
            body = self._lens_reason(kind).encode("utf-8")
            self.send_response(503)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return self.wfile.write(body)
        self._stream = True
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        ctype = b"image/jpeg" if kind == "vis" else b"image/png"
        last_ts = 0.0
        try:
            while True:
                with LOCK:
                    FRAME_COND.wait_for(lambda: FRAMES[kind][1] != last_ts, timeout=10)
                    data, ts = FRAMES[kind]
                    last_ts = ts
                if not data or time.time() - ts > 5:
                    continue
                self.wfile.write(b"--frame\r\nContent-Type: " + ctype +
                                 b"\r\nContent-Length: " + str(len(data)).encode() + b"\r\n\r\n")
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except Exception:
            return

    # ── LLM ──
    # ── 意图分流：研判 > 列举 > 其它 ──
    # 研判（要给结论和建议）必须**先判**：否则「近 1 分钟双光事件分析」会被「事件」
    # 两个字抢走，变成让它逐条复述台账，一个问题也答不出来。
    ANALYSIS_Q = ("分析", "研判", "判断", "评估", "对比", "比较", "量化", "结论",
                  "是否正常", "正常吗", "建议", "理由", "趋势")
    # 列举（只报台账）——不放「建议/判断」这类词，避免抢研判的活
    EVENT_Q = ("哪些事件", "什么事件", "事件", "监测到", "监测", "发生", "告警",
               "变化", "一分钟", "1分钟", "1 分钟", "最近", "刚才", "出现", "离开", "有人")

    @staticmethod
    def _is_analysis_q(q):
        # 「事件复核」要的是 confirm/reject 单词，不能被研判口径接管
        if "confirm" in q.lower() or "reject" in q.lower():
            return False
        return any(k in q for k in Handler.ANALYSIS_Q)

    @staticmethod
    def _is_event_q(q):
        return any(k in q for k in Handler.EVENT_Q)

    def _grounded(self, req):
        """给对话补实时 system 上下文，按意图走三种互斥口径。

        三种口径的事实源和输出契约都不一样，混用就会答错：
          研判（analysis）→ 双光窗口聚合 + 对比行；只出「判断/理由/建议」，不列事件
          列举（event）   → 事件台账；只报条目，禁止判断和建议
          其它（roster）  → 四节点分工/进程/模型
        事实都是请求现场取的（采样 2s 一次、SSH 名单 25s 一次缓存），
        不是构建期快照，所以答案会跟着实际状态变。req 里 ground=false 可关。
        """
        if not req.get("ground", True):
            return req
        msgs = req.get("messages") or [{"role": "user", "content": req.get("prompt", "")}]
        q = " ".join(str(m.get("content") or "") for m in msgs if m.get("role") == "user")
        # 事件复核是自包含请求（用户消息里已经带了这条事件和选项），
        # 再接地反而把无关台账塞进去，0.6B 就答不出 confirm/reject 了。
        if re.search(r"confirm|reject", q, re.I):
            out = dict(req)
            out["_intent"] = "review"
            return out
        try:
            ctx = roster_text()
        except Exception:
            ctx = ""
        try:
            r = recent_events(60)
            meta = {}
            evt = events_text(60, meta=meta)
        except Exception:
            r, evt, meta = None, "", {"n": 0}

        mode, parts, evd, zero = "roster", [ctx], None, ""
        if evt and self._is_analysis_q(q):
            secs = q_seconds(q, 60.0)
            ev, _err = build_evidence(secs)
            if ev:                            # 窗口采样不足就退回事件口径，不硬编
                mode = "analysis"
                # 与 /api/analyze/stream 用同一份事实，两条入口才不会给出两套数字
                parts = ["对比（近 %.0f 秒实测）：%s" % (secs, compare_line(ev))]
                evd = (ev, evidence_text(ev))
        if mode == "roster" and evt and self._is_event_q(q):
            mode, parts = "event", [evt]

        if mode == "analysis":
            rule = ""   # 事实与格式由 analysis_prompt 一次给全（见下方 messages 构造）
        elif mode == "event":
            n = meta.get("n") or 0
            if n == 1:
                # 单条不要写成「1)~1)」——0.6B 会把这行当格式错误而回「没有事件」。
                # 抬头（「共 N 条」）不交给模型：实测「小标题」「原样照抄这一行」它都丢，
                # 由页面上的台账条承担，模型只负责把条目抄准。
                rule = ("照抄上面 1) 那一条事件，原样保留时间和文字，不要改写；"
                        "不要加判断、不要加建议。")
            elif n > 1:
                rule = ("照抄上面 1) 到 %d) 的 %d 条事件，一条一行，原样保留时间和文字，"
                        "不要改写或合并；不要加判断、不要加建议。" % (n, n))
            else:
                # 空窗口不做「逐条复述」，否则它会把整块事实（含稳态那行）抄成一长句
                # （实测 20s 生成一句带冒号的长文本）。这里只给一句事实，让它总结。
                st = (r or {}).get("steady") or {}
                names = "、".join(c["name"] for c in (st.get("classes") or [])[:2]) or "无目标"
                # 空窗口没有可「总结」的东西，让它自由发挥反而出事：实测它会把
                # 「热斑 2.4 个/帧」说成「监测到 2.4 个热斑」，把稳态当成事件；
                # 而在 system+user 结构下还会把用户的问题原样吐回来。
                # 所以整条消息只给它这一句成品（实测一字不差，24 tok）。
                zero = "近一分钟无事件。当前持续监测 %s，热斑 %.1f 个/帧。" % (
                    names, st.get("blob_n_avg") or 0)
                parts, rule = [], ""
        else:
            if r:
                parts.append("近 60 秒：告警 %d 条、状态变化 %d 条、恢复 %d 条" % (
                    r["counts"]["alarm"], r["counts"]["change"], r["counts"]["recover"]))
            rule = "只依据以上事实回答；没列的就回答「数据里没有」，不要编造数字。"

        out = dict(req)
        if zero:
            out["messages"] = [{"role": "user", "content": zero + "\n把这句话原样说一遍。"}]
        elif mode == "analysis":
            # 和「近 1 分钟双光分析」chip 完全同构：单条 user 里放事实 + 格式。
            # 实测同一份规则放进 system，0.6B 会忽略格式甚至改写判断词；
            # 放进 user（跟它对 chip 的处理路径一致）才照做。
            out["messages"] = [{"role": "user", "content": analysis_prompt(ev)}]
        else:
            out["messages"] = [{
                "role": "system",
                "content": "\n\n".join(p for p in parts if p) + "\n" + rule,
            }] + msgs
        out["_intent"] = mode
        if mode == "event":
            out["_evt_text"] = evt           # 随 done 帧回传，页面显示为可核对的台账条
        elif mode == "analysis":
            out["_evd"], out["_evd_text"] = evd   # 研判回传的是证据条，不是事件条
        return out

    def _payload(self, req):
        msgs = req.get("messages")
        if not msgs:
            msgs = [{"role": "user", "content": req.get("prompt", "用一句话说明你的角色。")}]
        return {
            "model": "g4-edge-llm",
            "messages": msgs,
            "temperature": float(req.get("temperature", 0.2)),
            "max_tokens": int(req.get("max_tokens", 160)),
            "stream": False,
            "chat_template_kwargs": {"enable_thinking": False},
        }

    def _llm_once(self, req):
        payload = self._payload(self._grounded(req))
        try:
            t0 = time.monotonic()
            body = json.loads(fetch(LLM + "/v1/chat/completions", 120,
                                    json.dumps(payload).encode(),
                                    {"Content-Type": "application/json"}).decode("utf-8"))
            text = body["choices"][0]["message"]["content"].strip()
            tim = body.get("timings", {})
            tps = tim.get("predicted_per_second")
            push_event("info", "LLM 一次性推理 %.2fs %s" % (time.monotonic() - t0,
                                                          ("%.1f tok/s" % tps) if tps else ""))
            bump()
            return self._json({"ok": True, "text": text,
                               "wall_s": round(time.monotonic() - t0, 2),
                               "tok_s": round(tps, 2) if tps else None,
                               "usage": body.get("usage", {})})
        except Exception as exc:
            return self._json({"ok": False, "error": str(exc)[:300]}, 502)

    def _llm_stream(self, req):
        g = self._grounded(req)
        extra = {}
        if g.get("_evt_text"):
            extra["events_text"] = g["_evt_text"]      # 列举口径的台账条
        if g.get("_evd"):
            extra["evidence"] = g["_evd"]              # 研判口径的证据条
            extra["evidence_text"] = g.get("_evd_text") or ""
        if g.get("_intent"):
            extra["intent"] = g["_intent"]
        return self._llm_sse(self._payload(g), extra or None)

    def _analyze(self, req):
        """近 N 秒双光窗口分析：后端聚合事实 → 中心 LLM 只做解释。"""
        try:
            secs = float(req.get("seconds", 60))
        except Exception:
            secs = 60.0
        secs = max(20.0, min(300.0, secs))
        ev, err = build_evidence(secs)
        if err:
            return self._json({"ok": False, "error": err}, 503)
        prompt = analysis_prompt(ev)
        payload = self._payload({
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": int(req.get("max_tokens", 130)),
            "temperature": 0.2,
        })
        push_event("info", "双光窗口分析 → 中心 LLM（%.0fs / %d 采样）" % (
            ev["seconds"], ev["samples"]))
        return self._llm_sse(payload, {"evidence": ev, "evidence_text": evidence_text(ev),
                                       "intent": "analysis"})

    def _sse_verdict(self, payload, extra):
        """研判三行：非流式取完整结果，遇到模板占位就重试一次，再一次性发 done。

        不用流式的原因：三行只有 30 tok 左右，逐字输出没有观感收益，
        反而会把「理由：一句话」这种占位中间态先画到页面上（再被覆盖）。
        """
        self._stream = True
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        t0 = time.monotonic()
        p = dict(payload)
        p["stream"] = False
        text, tim, tries = "", None, 0
        try:
            for tries in (1, 2, 3):
                if tries > 1:
                    # 0.2 下它会把同一句占位原封不动再生一遍，升温才换得动
                    p["temperature"] = 0.45
                body = json.loads(fetch(LLM + "/v1/chat/completions", 120,
                                        json.dumps(p).encode(),
                                        {"Content-Type": "application/json"}).decode("utf-8"))
                tim = body.get("timings") or tim
                text = tidy_verdict((body["choices"][0]["message"].get("content") or "").strip())
                ev = (extra or {}).get("evidence")
                if ev:
                    text = enforce_verdict(text, ev)
                if not has_placeholder(text):
                    break
                push_event("info", "研判输出含模板占位，第 %d 次重试" % tries)
            if has_placeholder(text):
                # 三连败就删掉占位行，并明确告诉用户这行不是模型给的结论
                text = strip_placeholders(text) or "（本次模型未给出有效结论）"
                text += "\n（本次模型未给出理由与建议，量化依据见下方对比条）"
                push_event("warn", "研判三次均未给出理由/建议，已标注降级")
            wall = time.monotonic() - t0
            tps = (tim or {}).get("predicted_per_second")
            done_msg = {"done": True, "text": text, "wall_s": round(wall, 2),
                        "tok_s": round(tps, 2) if tps else None,
                        "timings": tim, "retries": max(0, tries - 1)}
            if extra:
                done_msg.update(extra)
            self.wfile.write(b"data: " + json.dumps(done_msg, ensure_ascii=False).encode() + b"\n\n")
            self.wfile.flush()
            push_event("info", "研判三行 %.2fs%s" % (
                wall, ("（重试 %d 次）" % (tries - 1)) if tries > 1 else ""))
            bump()
        except Exception as exc:
            try:
                self.wfile.write(b"data: " + json.dumps(
                    {"error": str(exc)[:200]}, ensure_ascii=False).encode() + b"\n\n")
                self.wfile.flush()
            except Exception:
                pass

    def _llm_sse(self, payload, extra):
        if (extra or {}).get("intent") == "analysis":
            return self._sse_verdict(payload, extra)
        payload["stream"] = True
        self._stream = True
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        t0 = time.monotonic()
        buf, chunks, timings = [], 0, None
        try:
            r = urllib.request.Request(
                LLM + "/v1/chat/completions",
                data=json.dumps(payload).encode(),
                headers={"Content-Type": "application/json"})
            with OPENER.open(r, timeout=180) as resp:
                for raw in resp:
                    line = raw.decode("utf-8", "ignore").strip()
                    if not line.startswith("data:"):
                        continue
                    chunk = line[5:].strip()
                    if chunk == "[DONE]":
                        break
                    try:
                        obj = json.loads(chunk)
                    except Exception:
                        continue
                    if obj.get("timings"):
                        timings = obj["timings"]
                    delta = (obj.get("choices") or [{}])[0].get("delta", {}).get("content") or ""
                    if delta:
                        buf.append(delta)
                        chunks += 1
                        self.wfile.write(b"data: " + json.dumps(
                            {"d": delta, "n": chunks}, ensure_ascii=False).encode() + b"\n\n")
                        self.wfile.flush()
            wall = time.monotonic() - t0
            tps = (timings or {}).get("predicted_per_second")
            text = "".join(buf)
            if (extra or {}).get("intent") == "analysis":
                text = tidy_verdict(text)     # 只整理换行，不改文字
            done_msg = {
                "done": True, "text": text, "wall_s": round(wall, 2),
                "tok_s": round(tps, 2) if tps else (round(chunks / wall, 2) if wall else None),
                "timings": timings}
            if extra:
                done_msg.update(extra)
            self.wfile.write(b"data: " + json.dumps(done_msg, ensure_ascii=False).encode() + b"\n\n")
            self.wfile.flush()
            push_event("info", "LLM 流式 %d 块 %.2fs%s" % (
                chunks, wall, (" · %.1f tok/s" % tps) if tps else ""))
            bump()
        except Exception as exc:
            try:
                self.wfile.write(b"data: " + json.dumps(
                    {"error": str(exc)[:200]}, ensure_ascii=False).encode() + b"\n\n")
                self.wfile.flush()
            except Exception:
                pass

    # ── 基准 ──
    def _bench(self, req):
        cases = req.get("cases") or [
            {"case": "role", "prompt": "一句话说明你在本实验中的角色。"},
            {"case": "confirm", "prompt": "事件：可见光检测到 mouse 0.78，面积 420 px²。是否需复核？只答 confirm 或 reject。"},
            {"case": "count", "prompt": "从 1 数到 5，用逗号分隔。"},
        ]
        out = []
        for c in cases:
            try:
                t0 = time.monotonic()
                body = json.loads(fetch(LLM + "/v1/chat/completions", 120, json.dumps({
                    "model": "g4-edge-llm",
                    "messages": [{"role": "user", "content": c["prompt"]}],
                    "temperature": 0, "max_tokens": 24, "stream": False,
                    "chat_template_kwargs": {"enable_thinking": False},
                }).encode(), {"Content-Type": "application/json"}).decode())
                tim = body.get("timings", {})
                out.append({"case": c["case"], "ok": True,
                            "wall_s": round(time.monotonic() - t0, 2),
                            "tok_s": round(tim.get("predicted_per_second", 0), 2),
                            "tokens": tim.get("predicted_n"),
                            "text": body["choices"][0]["message"]["content"].strip()[:120]})
            except Exception as exc:
                out.append({"case": c["case"], "ok": False, "error": str(exc)[:160]})
        ok = [c["tok_s"] for c in out if c["ok"] and c.get("tok_s")]
        median = sorted(ok)[len(ok) // 2] if ok else None
        push_event("info", "基准完成 %d/%d · 中位 %s tok/s" % (len(ok), len(out), median))
        bump()
        return self._json({"ok": True, "runs": out, "median_tok_s": median})


def main():
    ap = argparse.ArgumentParser()
    _host, _port = lab.dash_bind() if lab else ("127.0.0.1", 8899)
    ap.add_argument("--host", default=_host)
    ap.add_argument("--port", type=int, default=_port)
    args = ap.parse_args()

    threading.Thread(target=frame_grabber, daemon=True).start()
    threading.Thread(target=collector, daemon=True).start()
    threading.Thread(target=topology_worker, daemon=True).start()
    threading.Thread(target=roster_worker, daemon=True).start()
    push_event("info", "dashboard v2 启动 · %s:%d" % (args.host, args.port))

    handler = functools.partial(Handler, directory=str(HERE))
    srv = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    srv.daemon_threads = True
    print("G4 dashboard v2: http://%s:%d/  (Ctrl-C to stop)" % (args.host, args.port), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
