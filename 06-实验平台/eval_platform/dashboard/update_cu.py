#!/usr/bin/env python3
"""把「多任务能力并集（搭配3）」写入 data.json 并回注 index.html seed。"""
import json
import os
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data.json")
HTML = os.path.join(HERE, "index.html")

with open(DATA, encoding="utf-8") as f:
    d = json.load(f)

d["capability_union"] = {
    "title": "多任务能力并集（搭配3：RV 视觉 + RK 大模型）",
    "summary": ("RV1126B 跑视觉检测（YOLOv8n），RK3588 同时跑大模型（ResNet-50），"
                "两板并行、能力互补——单机任何一方都无法独立完成「实时视觉 + 大模型识别」。"),
    "models": [
        {"node": "RV1126B", "role": "视觉模型", "model": "YOLOv8n (RKNN)",
         "params": "~3.2M", "latency": "81ms · 8fps", "unit": "NPU 55%",
         "note": "目标检测，实时"},
        {"node": "RK3588", "role": "大模型", "model": "ResNet-50 (onnxruntime)",
         "params": "~25.6M（8×）", "latency": "122ms · 8.2fps",
         "unit": "CPU 747%（~7.5核）· 302MB RAM", "note": "图像分类，FP32"},
    ],
    "conclusion": [
        "RV 单机（2 TOPS NPU / 2GB RAM）跑不动 ResNet-50：NPU 仅 INT8、内存装不下 100MB FP32 大模型。",
        "RK 单机拿不到相机实时帧（相机在 RV 上）。",
        "并集：RV 视觉 + RK 大模型并行运行，互不影响，能力互补。",
    ],
}

d["meta"]["generated_utc"] = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
d["meta"]["source"] = d["meta"]["source"] + " + 多任务能力并集（RV 视觉 + RK 大模型）"

with open(DATA, "w", encoding="utf-8") as f:
    json.dump(d, f, ensure_ascii=False, indent=2)

with open(HTML, encoding="utf-8") as f:
    html = f.read()
marker = '<script id="seed" type="application/json">'
start = html.index(marker) + len(marker)
end = html.index('</script>', start)
html = html[:start] + json.dumps(d, ensure_ascii=False, indent=2) + html[end:]
with open(HTML, "w", encoding="utf-8") as f:
    f.write(html)

print("updated; capability_union keys:", list(d["capability_union"].keys()))
