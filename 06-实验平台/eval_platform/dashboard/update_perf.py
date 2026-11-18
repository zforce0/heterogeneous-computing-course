#!/usr/bin/env python3
"""把「多核互联异构策略/算法性能对比」数据写入 data.json 并回注 index.html seed。"""
import json
import os
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data.json")
HTML = os.path.join(HERE, "index.html")

with open(DATA, encoding="utf-8") as f:
    d = json.load(f)

d["perf_compare"] = {
    "title": "多核互联异构策略/算法性能对比",
    "summary": ("单节点 RV 裸推理 vs 互联异构协同（RV 推理 + RK 规则二次确认），"
                "量化协同带来的二次确认与链路可靠性增益，以及附加的时延/带宽开销。"),
    "metrics_note": ("综合时延 = 中位 RV 推理 + 中位传输 RTT + 中位 RK 复核；"
                     "准确性未引入（无标注集，规划 E12 标 N/A），以 RK 二次确认的确认率近似。"),
    "sample": "perf_sample_001 · 15s · policy=all · 124 事件",
    "baseline": {
        "name": "单节点裸推理（RV 独跑）",
        "infer_ms": 76.0,
        "e2e_ms": 76.0,
        "rtt_p95_ms": None,
        "reconfirm_rate": None,
        "reliability": "无 ACK/重试，断链即丢",
    },
    "collab": {
        "name": "互联异构协同（RV 推理 + RK 规则复核）",
        "infer_ms": 76.0,
        "rtt_p95_ms": 1.06,
        "reconf_ms": 0.1143,
        "e2e_ms": 77.01,
        "overhead_ms": 1.01,
        "overhead_pct": 1.33,
        "reconfirm_rate": 99.2,
        "reliability": "at-least-once + 幂等去重（E11 重试11/去重11 不重不漏）",
    },
}

d["meta"]["generated_utc"] = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
d["meta"]["source"] = d["meta"]["source"] + " + 策略/算法性能对比（综合推理时延）"

with open(DATA, "w", encoding="utf-8") as f:
    json.dump(d, f, ensure_ascii=False, indent=2)

# re-inject into index.html seed
with open(HTML, encoding="utf-8") as f:
    html = f.read()
marker = '<script id="seed" type="application/json">'
start = html.index(marker) + len(marker)
end = html.index('</script>', start)
html = html[:start] + json.dumps(d, ensure_ascii=False, indent=2) + html[end:]
with open(HTML, "w", encoding="utf-8") as f:
    f.write(html)

print("updated data.json + index.html seed; perf_compare keys:", list(d["perf_compare"].keys()))
