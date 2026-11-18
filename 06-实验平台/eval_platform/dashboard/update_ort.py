#!/usr/bin/env python3
"""把 onnxruntime CPU 真实模型推理结果并入 backend_bench，回注 index.html seed。"""
import json
import os
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data.json")
HTML = os.path.join(HERE, "index.html")

with open(DATA, encoding="utf-8") as f:
    d = json.load(f)

bb = d["backend_bench"]

# 追加 onnxruntime 真实模型推理条目
bb["backends"].append({
    "id": "S2c", "node": "RK3588", "unit": "CPU (onnxruntime)",
    "workload": "mobilenetv2.onnx（FP32，224×224）",
    "result": "37.57ms · 26.6 FPS", "util": "8 核 CPU（仅 CPU EP，无 GPU/NPU）",
    "kind": "真实模型推理",
})

# 追加部署信息
bb["onnxruntime"] = {
    "version": "onnxruntime 1.17.3 (aarch64)",
    "providers": ["CPUExecutionProvider"],
    "model": "mobilenetv2-7.onnx（14.2MB，从 Mac 下载转传）",
    "note": "RK3588 无网络，通过 Mac 下载 aarch64 wheel（onnxruntime+numpy+protobuf+sympy 等 9 个）离线安装；标准 onnxruntime 在 ARM 上无 Mali-GPU / NPU 执行提供者，故 CPU-only。",
}

# 更新结论
bb["conclusion"] = [
    "NPU(RKNN) 跑真实 NN 最快：mobilenet_v1 2.3ms/次（430 FPS），约为 onnxruntime CPU 的 16 倍（26.6 FPS）。",
    "CPU(onnxruntime) 跑 mobilenetv2 FP32 = 37.57ms（26.6 FPS），标准 wheel 仅 CPU EP，未启用 Mali GPU / NPU。",
    "GPU(Mali) 擅长大矩阵：GEMM 1624 GFLOPS ≈ CPU 的 677 倍，但标准 onnxruntime 无 Mali 执行提供者，需经 OpenCV UMat/OpenCL 或自建 EP。",
    "结论：RK3588 真实 NN 推理优先 NPU(RKNN)；onnxruntime 只兜底 CPU；GPU 作大矩阵加速补充。",
]

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

print("updated; backends:", len(bb["backends"]), "; onnxruntime:", bb.get("onnxruntime", {}).get("version"))
