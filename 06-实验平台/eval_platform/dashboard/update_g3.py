#!/usr/bin/env python3
"""把 G3 互联异构计算（E09/E10/E11）结果归档进 dashboard 数据源并回注 seed。"""
import json
import os
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data.json")
HTML = os.path.join(HERE, "index.html")

with open(DATA, encoding="utf-8") as f:
    d = json.load(f)

# ---- stages ----
for s in d["stages"]:
    if s["id"] == "G3":
        s["status"] = "done"
        s["note"] = "E09 证据协议端到端 + E10 策略消融 + E11 链路受限；双端对账一致"
    elif s["id"] == "G1":
        s["note"] = ("E00–E04 完成；E09/E10/E11 完成（本轮 G3）；E08/E13 部分；"
                     "E05/E06/E14 未跑；E07/E12 N/A")

# ---- experiments ----
for e in d["experiments"]:
    if e["id"] == "E09":
        e["status"] = "done"
        e["result"] = "RV→RK 长度前缀 TCP 证据协议 + RK 规则二次确认 + ACK 幂等对账：sent 164/acked 164/confirmed 164，RTT 0.82ms"
    elif e["id"] == "E10":
        e["status"] = "done"
        e["result"] = "四策略消融：person 带宽 -24.1%、roi_top1 -22.4%、highconf -6.5%"
    elif e["id"] == "E11":
        e["status"] = "done"
        e["result"] = "应用层链路仿真：时延150ms 零丢包；时延100ms+ACK丢包15%+期限120ms → 重试11/去重11 全对账"

# ---- completion ----
c = d["completion"]
c["counts"] = {"completed": 8, "partial": 2, "not_run": 3, "na": 2, "total": 15}
c["completed"] = ["E00", "E01", "E02", "E03", "E04", "E09", "E10", "E11"]
c["partial"] = ["E08", "E13"]
c["not_run"] = ["E05", "E06", "E14"]
c["per_experiment"]["E09"] = {
    "status": "completed",
    "evidence": "长度前缀 TCP 证据协议（run/node/event ID + 模型版本 + deadline）RV→RK，"
                "RK 规则二次确认回 verdict，幂等去重；sent164/acked164/confirmed164，RTT 0.82ms，双端对账一致",
}
c["per_experiment"]["E10"] = {
    "status": "completed",
    "evidence": "四种采样策略消融：person -24.1% 带宽、roi_top1 -22.4%、highconf -6.5%，事件数不变但载荷字节显著下降",
}
c["per_experiment"]["E11"] = {
    "status": "completed",
    "evidence": "RK/RV 内核均缺 sch_netem，改用应用层链路仿真：时延150ms+自适应期限 100% 交付；"
                "时延100ms+回程ACK丢包15%+期限120ms → retries11/duplicate11，at-least-once+幂等全对账",
}
now = datetime.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
c["generated_utc"] = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
c["new_runs"] = [
    now + "_E09_evidence_protocol",
    now + "_E10_policy_ablation",
    now + "_E11_link_limited",
]
c["honest_note"] = ("按规划 7.2 与 9.2，未测值一律 N/A，禁止默认填 0；短测与正式长测分表，未达标结果同样归档。"
                    "G3 互联异构计算已达成「两板协议能对账」：E09/E10/E11 完成并双端对账一致。")

# ---- runs ----
d["runs"].append({
    "id": now + "_E09_evidence_protocol",
    "experiment": "E09", "status": "completed", "node": "rv1126b<->rk3588",
    "findings": {
        "protocol": "长度前缀 TCP 帧 + run/node/event ID + 模型版本 + deadline",
        "sender": "sent164/acked164/confirmed164/timeout0/retries0",
        "server": "received164/unique164/duplicate0/confirmed164",
        "rtt_ms": "min0.52 median0.82 p95 1.02 max1.49",
        "reconcile": "unique 164=164；duplicate 0=0；一致",
    },
})
d["runs"].append({
    "id": now + "_E10_policy_ablation",
    "experiment": "E10", "status": "completed", "node": "rv1126b->rk3588",
    "findings": {
        "all": "123 事件 49078B",
        "person": "123 事件 37263B (-24.1%)",
        "highconf": "124 事件 45884B (-6.5%)",
        "roi_top1": "124 事件 38059B (-22.4%)",
    },
})
d["runs"].append({
    "id": now + "_E11_link_limited",
    "experiment": "E11", "status": "completed", "node": "rv1126b->rk3588",
    "findings": {
        "e11a_delay150": "RTT151ms 期限755ms sent79/acked79 零丢包",
        "e11b_loss": "RTT101ms 期限120ms sent104/unique93 retries11/dup11 全对账",
        "note": "内核无 sch_netem，应用层仿真时延+回程ACK丢包",
    },
})

# ---- interconnect section ----
d["interconnect"] = {
    "title": "互联异构协同（G3 · E09–E11）",
    "summary": ("RV1126B 端 YOLOv8(RKNN) 推理产出检测事件，按长度前缀 TCP 证据协议打包发 RK3588，"
                "RK 侧规则二次确认回 verdict；统一 run_id、同钟 RTT 建期限、幂等去重、双端对账一致。"),
    "arch": [
        {"stage": "感知推理", "node": "RV1126B", "desc": "YOLOv8n(RKNN) 实时检测 bbox/score/infer_ms", "tech": "yolo_live + live_boxes.json"},
        {"stage": "证据打包", "node": "RV1126B", "desc": "长度前缀 + run/node/event ID + 模型版本 + deadline", "tech": "evidence_sender.py"},
        {"stage": "传输", "node": "RV→RK", "desc": "千兆直连 TCP:9100", "tech": "L2 直连"},
        {"stage": "二次确认", "node": "RK3588", "desc": "规则复核 ROI 几何/置信度回 confirm/reject", "tech": "evidence_server.py (rule)"},
        {"stage": "对账", "node": "双端", "desc": "sent/ACK/duplicate/timeout 统计一致", "tech": "per-run 对账"},
    ],
    "protocol": {
        "framing": "4 字节大端长度前缀 + UTF-8 JSON",
        "fields": ["run_id", "node", "event_id", "model", "model_version", "deadline_ms", "seq", "infer_ms", "detections[]"],
        "idempotency": "(run_id, event_id) 去重，重发返回缓存 ACK 并计 duplicate",
        "deadline": "同钟(time.monotonic) RTT ×5，下限 200ms",
        "retries": 3,
        "clock_note": "三端墙钟漂移（Mac 2026-11-06 / RK 2026-04-15 / RV 2026-10-13），协议用单调钟+RTT 规避绝对时间依赖",
    },
    "e09": {
        "status": "completed", "duration_s": 20,
        "sender": {"sent": 164, "unique_sent": 164, "acked": 164, "confirmed": 164,
                   "timeout": 0, "retries": 0, "dropped": 0},
        "server": {"received": 164, "unique": 164, "duplicate": 0, "confirmed": 164, "rejected": 0},
        "rtt_ms": {"min": 0.52, "median": 0.82, "p95": 1.02, "max": 1.49},
        "reconcile": "unique 164=164；confirmed 164=164；duplicate 0=0；无丢包无重复",
    },
    "e10": {
        "status": "completed",
        "policies": [
            {"policy": "all", "events": 123, "bytes_out": 49078, "rtt_median_ms": 0.82},
            {"policy": "person", "events": 123, "bytes_out": 37263, "rtt_median_ms": 0.76},
            {"policy": "highconf", "events": 124, "bytes_out": 45884, "rtt_median_ms": 0.81},
            {"policy": "roi_top1", "events": 124, "bytes_out": 38059, "rtt_median_ms": 0.75},
        ],
        "conclusion": "person -24.1% 带宽、roi_top1 -22.4%、highconf -6.5%；场景常含 person 故事件数不变，载荷字节显著下降",
    },
    "e11": {
        "status": "completed",
        "note": "RK/RV 内核均缺 sch_netem，改用应用层链路仿真（时延 + 回程 ACK 丢包），更直接考验协议自身重试幂等",
        "scenarios": [
            {"id": "baseline", "desc": "千兆直连", "rtt_median_ms": 0.82, "sent": 164, "timeout": 0, "retries": 0},
            {"id": "e11a", "desc": "时延150ms+自适应期限", "rtt_median_ms": 151.26, "deadline_ms": 755,
             "sent": 79, "acked": 79, "timeout": 0, "retries": 0, "dropped": 0},
            {"id": "e11b", "desc": "时延100ms+ACK丢包15%+期限120ms", "rtt_median_ms": 101.2, "deadline_ms": 120,
             "sent": 104, "unique_sent": 93, "acked": 93, "duplicate_acked": 11, "retries": 11, "timeout": 0, "dropped": 0},
        ],
        "reconcile_e11b": {"unique": "93=93", "frames": "104=104", "duplicate": "11=11",
                           "ack_dropped": "11=11", "acked": "confirm82+dup11=93=unique"},
    },
    "verdict_rules": [
        {"rule": "invalid_bbox", "desc": "宽或高 ≤ 0 → reject"},
        {"rule": "area_too_small", "desc": "ROI 面积 < 400 px² → reject"},
        {"rule": "aspect_out_of_range", "desc": "宽高比 ∉ [0.1, 10] → reject"},
        {"rule": "person_score_low", "desc": "person 且 score < 0.45 → reject"},
        {"rule": "score_low", "desc": "其他类 score < 0.30 → reject"},
        {"rule": "geometry+score_ok", "desc": "以上皆通过 → confirm"},
    ],
}

# ---- meta ----
d["meta"]["generated_utc"] = c["generated_utc"]
d["meta"]["source"] = d["meta"]["source"] + " + G3 互联异构计算（E09/E10/E11 证据协议）"

with open(DATA, "w", encoding="utf-8") as f:
    json.dump(d, f, ensure_ascii=False, indent=2)

# ---- re-inject into index.html seed ----
with open(HTML, encoding="utf-8") as f:
    html = f.read()
marker = '<script id="seed" type="application/json">'
start = html.index(marker) + len(marker)
end = html.index('</script>', start)
seed = json.dumps(d, ensure_ascii=False, indent=2)
html = html[:start] + seed + html[end:]
with open(HTML, "w", encoding="utf-8") as f:
    f.write(html)

print("updated data.json + index.html seed")
print("counts:", c["counts"])
print("interconnect keys:", list(d["interconnect"].keys()))
