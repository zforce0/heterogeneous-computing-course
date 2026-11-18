#!/usr/bin/env python3
"""Build the timestamped key-frame timeline for the heterogeneous experiment.

Key frames are the 4 saved vis/IR pairs produced by the RV1126B on-device
dual_demo (YOLOv8n RKNN + IR/RGB differential saliency cascade) on 2026-10-12.
Each pair is matched to the nearest event in events_ai.jsonl by ts_ms and then
sent to the M1 center LLM node for semantic second confirmation.
"""
import base64
import datetime
import html
import json
import urllib.request
import zoneinfo
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAMPLES = ROOT / "eval_platform/dashboard/samples"
RUN = ROOT / "eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous"
TZ = zoneinfo.ZoneInfo("Asia/Shanghai")
FRAMES = ["224914_889", "224915_968", "224916_064", "224916_158"]
API = "http://127.0.0.1:11434/api/generate"
MODEL = "hetero-edge-llm:v1"

EDGE_NODES = {
    "rv1126b": {"model": "YOLOv8n (RKNN) + 双光差分显著级联", "role": "双光采集 / 轻量视觉推理"},
    "rk3588": {"model": "ResNet-50 (RKNN FP16 / NPU) 或 ROI 复核", "role": "边缘视觉复核"},
    "ascend": {"model": "YOLOv8 OM (CANN/ACL, INT8)", "role": "昇腾视觉加速（规划）"},
}


def llm_verdict(event):
    payload = {
        "model": MODEL,
        "stream": False,
        "think": False,
        "keep_alive": "5m",
        "options": {"num_predict": 110, "temperature": 0.2},
        "prompt": "请按固定三段格式处理下面这条端侧视觉检测事件：\n"
                  + json.dumps(event, ensure_ascii=False),
    }
    req = urllib.request.Request(
        API, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=180) as r:
        return json.loads(r.read().decode("utf-8")).get("response", "").strip()


def b64(path):
    return "data:image/jpeg;base64," + base64.b64encode(path.read_bytes()).decode()


def main():
    rows = [json.loads(l) for l in (SAMPLES / "events_ai.jsonl").read_text().splitlines() if l.strip()]
    frames = []
    for f in FRAMES:
        hh, mm, ss, ms = int(f[:2]), int(f[2:4]), int(f[4:6]), int(f[7:])
        target = datetime.datetime(2026, 10, 12, hh, mm, ss, ms * 1000, tzinfo=TZ).timestamp() * 1000
        ev = min(rows, key=lambda r: abs(r["ts_ms"] - target))
        dt = datetime.datetime.fromtimestamp(ev["ts_ms"] / 1000, TZ)
        vis = f"det_20261012_{f}_vis.jpg"
        ir = f"det_20261012_{f}_ir.jpg"
        item = {
            "pair_id": f,
            "frame_time_local": dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + " +0800",
            "frame_time_utc": datetime.datetime.fromtimestamp(
                ev["ts_ms"] / 1000, datetime.timezone.utc
            ).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z",
            "ts_ms": ev["ts_ms"],
            "match_delta_ms": round(ev["ts_ms"] - target, 1),
            "event_id": ev["id"],
            "track": ev["track"],
            "node": "rv1126b",
            "model": EDGE_NODES["rv1126b"]["model"],
            "vis_frame": vis,
            "ir_frame": ir,
            "semantic": ev["semantic"],
            "area_px": ev["area_px"],
            "ir_box": ev["ir_box"],
            "centroid": ev["centroid"],
            "temp": ev["temp"],
            "saliency": ev["saliency"],
            "persistence_s": ev["temporal"]["persistence_s"],
            "llm_verdict": llm_verdict(ev),
            "_vis_b64": b64(SAMPLES / vis),
            "_ir_b64": b64(SAMPLES / ir),
        }
        frames.append(item)
        print(f"[{f}] {item['frame_time_local']}  {item['semantic']['label']} "
              f"score={item['semantic']['score']}  -> {item['llm_verdict'][:60]}")

    doc = {
        "schema_version": "keyframe-timeline-v1",
        "run_id": "20261113T145438Z_G4_m1_ascend_heterogeneous",
        "generated_local": datetime.datetime.now(TZ).strftime("%Y-%m-%d %H:%M:%S %z"),
        "clock_note": "帧时间取自 events_ai.jsonl 的 ts_ms（UTC 毫秒），换算为 Asia/Shanghai；"
                      "文件名内嵌时间戳与事件时间相差 1–2 ms，属同一帧落盘开销。"
                      "三端墙钟漂移见 G3 report 第 3 节，跨设备仍以单调钟 + RTT 为准。",
        "source": "RV1126B dual_demo 20s 短测，2026-10-12 22:49:00.816 → 22:49:16.157 (+0800)，"
                  "共 38 个检测事件、4 组已落盘 vis/IR 配对关键帧",
        "edge_nodes": EDGE_NODES,
        "center_llm": {"node": "MacBook Air M1", "model": MODEL, "runtime": "Ollama/Metal"},
        "keyframes": [{k: v for k, v in f.items() if not k.startswith("_")} for f in frames],
        "honest_note": "关键帧与检测事件均为 2026-10-12 板端实跑归档数据；"
                       "2026-11-13 本轮未能在 RK3588 / RV1126B / 昇腾盒子上现场重跑"
                       "（实验网无路由、SSH 别名缺失），故标注为 archived，非本轮新采集。",
    }
    (RUN / "keyframes.json").write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n")

    # self-contained HTML timeline
    cards = []
    for i, f in enumerate(frames, 1):
        t = f["temp"]
        s = f["semantic"]
        cards.append(f"""
        <article class="card">
          <div class="cardhead">
            <span class="idx">KF-{i:02d}</span>
            <time>{html.escape(f['frame_time_local'])}</time>
            <span class="tag">{html.escape(f['frame_time_utc'])}</span>
          </div>
          <div class="imgs">
            <figure><img src="{f['_vis_b64']}" alt="visible"><figcaption>可见光 · {html.escape(f['vis_frame'])}</figcaption></figure>
            <figure><img src="{f['_ir_b64']}" alt="thermal"><figcaption>热成像 · {html.escape(f['ir_frame'])}</figcaption></figure>
          </div>
          <div class="meta">
            <div><span>节点 / 模型</span><b>RV1126B · YOLOv8n RKNN 双光级联</b></div>
            <div><span>事件 id / track</span><b>{f['event_id']} / {f['track']}</b></div>
            <div><span>语义标签</span><b>{html.escape(str(s['label']))} · score {s['score']}</b></div>
            <div><span>ROI / 面积</span><b>{f['ir_box']} · {f['area_px']} px²</b></div>
            <div><span>热特征</span><b>t_max {t['t_max']}°C · ΔT {t['delta_t']}K</b></div>
            <div><span>持续 / 对齐误差</span><b>{f['persistence_s']}s · {f['match_delta_ms']} ms</b></div>
          </div>
          <div class="verdict"><span>M1 中心 LLM 二次确认</span><p>{html.escape(f['llm_verdict']).replace(chr(10), '<br>')}</p></div>
        </article>""")

    page = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>互联异构计算 · 端侧视觉关键帧时间线</title>
<style>
:root{--bg:#f7f8fa;--card:#fff;--line:#e3e6ec;--ink:#1c2230;--muted:#6b7280;--accent:#2f5fd0;--soft:#eef2fb}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);
font:-apple-system,BlinkMacSystemFont,"PingFang SC","Helvetica Neue",Arial,sans-serif;line-height:1.6}
.wrap{max-width:1180px;margin:0 auto;padding:36px 24px 64px}
h1{font-size:26px;margin:0 0 6px;letter-spacing:.2px}.sub{color:var(--muted);font-size:14px;margin:0 0 22px}
.banner{background:var(--soft);border:1px solid #d6e0f7;border-radius:10px;padding:14px 18px;color:#33406b;font-size:13px;margin-bottom:26px}
.timeline{position:relative;padding-left:26px}
.timeline:before{content:"";position:absolute;left:7px;top:8px;bottom:8px;width:2px;background:var(--line)}
.card{position:relative;background:var(--card);border:1px solid var(--line);border-radius:12px;
padding:18px 20px;margin-bottom:24px;box-shadow:0 1px 3px rgba(20,30,60,.05)}
.card:before{content:"";position:absolute;left:-24px;top:26px;width:11px;height:11px;border-radius:50%;
background:var(--accent);box-shadow:0 0 0 4px var(--bg)}
.cardhead{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px}
.idx{background:var(--accent);color:#fff;font-size:12px;font-weight:700;padding:3px 9px;border-radius:5px}
time{font-size:17px;font-weight:650;font-variant-numeric:tabular-nums}
.tag{color:var(--muted);font-size:12px;font-variant-numeric:tabular-nums}
.imgs{display:grid;grid-template-columns:1fr 1fr;gap:14px}
figure{margin:0;background:#fafbfc;border:1px solid var(--line);border-radius:8px;overflow:hidden}
img{display:block;width:100%;height:auto}
figcaption{font-size:11px;color:var(--muted);padding:6px 9px;border-top:1px solid var(--line);word-break:break-all}
.meta{display:grid;grid-template-columns:repeat(3,1fr);gap:10px 18px;margin:16px 0;font-size:13px}
.meta div{display:flex;flex-direction:column;gap:1px}.meta span{color:var(--muted);font-size:11px}.meta b{font-weight:600}
.verdict{background:#f4f7ff;border-left:3px solid var(--accent);border-radius:0 8px 8px 0;padding:10px 14px;font-size:13px}
.verdict span{color:var(--accent);font-weight:650;font-size:12px}.verdict p{margin:4px 0 0}
.foot{color:var(--muted);font-size:12px;margin-top:28px;border-top:1px solid var(--line);padding-top:14px}
@media(max-width:720px){.imgs,.meta{grid-template-columns:1fr}}
</style></head><body><div class="wrap">
<h1>互联异构计算 · 端侧视觉关键帧时间线</h1>
<p class="sub">RV1126B 板端视觉模型产出 → MacBook Air M1 中心 LLM 语义二次确认 ｜ run 20261113T145438Z_G4</p>
<div class="banner"><b>数据口径：</b>下列 4 组关键帧为 2026-10-12 22:49:00–22:49:16 (+0800) 板端实跑归档，
由 RV1126B 的 YOLOv8n(RKNN) + 双光差分显著级联产出，共 38 个检测事件、4 组落盘配对帧。
时间戳取自 events_ai.jsonl 的 <code>ts_ms</code>，与帧文件名内嵌时间相差 1–2 ms。
本轮（2026-11-13）实验网无路由、SSH 别名缺失，未在三台板端现场重跑，LLM 二次确认为本机新增实测。</div>
<div class="timeline">""" + "".join(cards) + """</div>
<div class="foot">时钟说明：三端墙钟曾实测漂移（RK 约 7 个月 / RV 约 24 天），跨设备协同仍以单调钟 + RTT 建期限；
本页时间为板端事件钟换算的本地墙钟，仅用于关键帧定位，不作为跨设备 RTT 依据。<br>
热成像为相对辐射强度显示；未取得标定协议前 <code>thermal_mode=relative</code>，摄氏度字段不作为绝对温度结论。</div>
</div></body></html>"""
    (RUN / "keyframes.html").write_text(page)
    print("\nwrote", RUN / "keyframes.json")
    print("wrote", RUN / "keyframes.html")


if __name__ == "__main__":
    main()
