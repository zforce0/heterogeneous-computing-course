#!/usr/bin/env python3
"""把 ResNet-50 转 RKNN 并在 NPU 跑通的结果并入 backend_bench，回注 seed。"""
import json
import os
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data.json")
HTML = os.path.join(HERE, "index.html")

with open(DATA, encoding="utf-8") as f:
    d = json.load(f)

bb = d["backend_bench"]

# 追加 ResNet-50 NPU 条目
bb["backends"].append({
    "id": "S1b", "node": "RK3588", "unit": "NPU (RKNN)",
    "workload": "resnet50.rknn（FP16，224×224，自转）",
    "result": "32.6ms 中位 · ~30 FPS", "util": "NPU Core0 ~60%（单核）",
    "kind": "真实模型推理（自转）",
})

# 追加转换信息
bb["resnet50_rknn"] = {
    "model": "resnet50-v2-7.onnx → resnet50.rknn（52MB，FP16）",
    "toolkit": "rknn-toolkit2 1.5.0（匹配板端 librknnrt 1.5.0）",
    "env": "Apple Silicon 上 x86_64 Docker（Ubuntu20.04 + Python3.8 模拟），onnx 动态 batch 已固定为 1",
    "compare": "同模型 CPU(onnxruntime FP32) 122ms → NPU(FP16) 32.6ms，约 3.7× 提速；INT8 可到 ~10ms（约 12×，官方 99 FPS）",
}

# 更新结论
bb["conclusion"].append(
    "ResNet-50 自转 RKNN（FP16）在 NPU 上 32.6ms，比同模型 CPU 122ms 快约 3.7×；INT8 量化可再提速至 ~10ms。"
)

d["meta"]["generated_utc"] = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

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

print("updated; backends:", len(bb["backends"]))
