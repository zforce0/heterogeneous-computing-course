#!/usr/bin/env python3
"""onnxruntime 在 RK3588 上的推理验证 + 基准（CPU, XNNPACK）。
用法: python3 ort_test.py [model.onnx] [iters] [H] [W]
"""
import sys
import numpy as np
import onnxruntime as ort
import time

MODEL = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mobilenetv2.onnx"
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 50
H = int(sys.argv[3]) if len(sys.argv) > 3 else 224
W = int(sys.argv[4]) if len(sys.argv) > 4 else 224

# 会话选项：尝试启用 XNNPACK（aarch64 CPU 加速）
so = ort.SessionOptions()
so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
providers = ort.get_available_providers()
print("available providers:", providers)

sess = ort.InferenceSession(MODEL, sess_options=so)
inp = sess.get_inputs()[0]
out = sess.get_outputs()[0]
print("input :", inp.name, inp.shape, inp.type)
print("output:", out.name, out.shape, out.type)

# 按模型输入 shape 构造随机输入（NCHW float32）
shape = inp.shape
n = shape[0] if isinstance(shape[0], int) else 1
c = shape[1] if isinstance(shape[1], int) else 3
ih = shape[2] if isinstance(shape[2], int) else H
iw = shape[3] if isinstance(shape[3], int) else W
x = np.random.rand(n, c, ih, iw).astype(np.float32)

# 预热
_ = sess.run(None, {inp.name: x})

# 基准
t0 = time.time()
for _ in range(ITERS):
    r = sess.run(None, {inp.name: x})
t1 = time.time()
avg_ms = (t1 - t0) / ITERS * 1000
print("avg inference: %.2f ms  (%.1f FPS)" % (avg_ms, 1000.0 / avg_ms))
print("output shape:", r[0].shape, "dtype:", r[0].dtype)
print("top1 class:", int(np.argmax(r[0][0])))

