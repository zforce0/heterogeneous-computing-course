#!/usr/bin/env python3
"""把「RK3588 算力单元推理基准（NPU/CPU/GPU）」写入 data.json 并回注 index.html seed。"""
import json
import os
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data.json")
HTML = os.path.join(HERE, "index.html")

with open(DATA, encoding="utf-8") as f:
    d = json.load(f)

d["backend_bench"] = {
    "title": "RK3588 算力单元推理基准（NPU / CPU / GPU）",
    "summary": ("对比 RV NPU 基线（YOLOv8n 检测）与 RK3588 三种算力单元（NPU/CPU/GPU）的推理计算能力，"
                "评估把 RK3588 异构算力用于推理的潜力。"),
    "note": ("RK3588 无网络 / 无 ONNX 模型 / 无 Python ML 栈：NPU 跑真实 mobilenet_v1.rknn（INT8 224×224）；"
             "CPU/GPU 用 OpenCV 优化内核（卷积+GEMM，CNN 主导算子，FP32）做同构对比。"),
    "baseline": {
        "node": "RV1126B", "unit": "NPU", "model": "YOLOv8n (RKNN)",
        "latency_ms": 82, "fps": "~8-9", "util": "NPU 56% @950MHz",
        "note": "真实目标检测 640×480",
    },
    "backends": [
        {"id": "S1", "node": "RK3588", "unit": "NPU",
         "workload": "mobilenet_v1.rknn（INT8，224×224）",
         "result": "2.30ms/次 · 430 FPS", "util": "NPU Core0 ~80%（单核）", "kind": "真实推理"},
        {"id": "S2", "node": "RK3588", "unit": "CPU",
         "workload": "OpenCV 卷积 512×512×3（FP32）",
         "result": "1.53ms · 9.2 GFLOPS", "util": "单线程", "kind": "推理计算"},
        {"id": "S2b", "node": "RK3588", "unit": "CPU",
         "workload": "OpenCV GEMM 512³（FP32）",
         "result": "110.5ms · 2.4 GFLOPS", "util": "单线程（未启用 BLAS）", "kind": "推理计算"},
        {"id": "S3", "node": "RK3588", "unit": "GPU (Mali-G610)",
         "workload": "OpenCL 卷积 512×512×3（FP32）",
         "result": "2.06ms · 6.9 GFLOPS", "util": "~50% 突发", "kind": "推理计算"},
        {"id": "S3b", "node": "RK3588", "unit": "GPU (Mali-G610)",
         "workload": "OpenCL GEMM 512³（FP32）",
         "result": "0.165ms · 1624 GFLOPS", "util": "~50% 突发 @400-500MHz", "kind": "推理计算"},
    ],
    "conclusion": [
        "NPU(RKNN) 跑真实 NN 最快：mobilenet_v1 2.3ms/次（430 FPS），是 RK3588 推理首选。",
        "GPU(Mali) 擅长大规模矩阵乘：GEMM 1624 GFLOPS ≈ CPU 的 677 倍，适合注意力/大 FC 层；但小卷积因数据搬运开销反而不如 CPU（6.9 vs 9.2 GFLOPS）。",
        "CPU 最弱：GEMM 2.4 GFLOPS（OpenCV 默认单线程，未启用 BLAS），适合兜底/小算子。",
        "结论：RK3588 用于推理应优先 NPU，GPU 作大矩阵加速补充，CPU 兜底。",
    ],
}

d["meta"]["generated_utc"] = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
d["meta"]["source"] = d["meta"]["source"] + " + RK3588 算力单元推理基准（NPU/CPU/GPU）"

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

print("updated; backend_bench keys:", list(d["backend_bench"].keys()))
