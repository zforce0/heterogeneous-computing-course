"""验证 yolov8n_person.onnx: 与 C++ 端一致的 letterbox 预处理 + YOLOv8 后处理.

用法: venv/bin/python verify_onnx.py [img1 img2 ...]
不传参数时默认测 captures/images/000.jpg 及其半身裁剪.
"""
import sys

import cv2
import numpy as np
import onnxruntime as ort

ONNX = "yolov8n_person.onnx"
INPUT = 640
CONF = 0.25
IOU = 0.45


def letterbox(bgr):
    """等比缩放 + 灰边(114)填充到 640x640, 返回 (tensor[1,3,640,640], params)."""
    h, w = bgr.shape[:2]
    scale = min(INPUT / w, INPUT / h)
    new_w, new_h = int(round(w * scale)), int(round(h * scale))
    resized = cv2.resize(bgr, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
    pad_x, pad_y = (INPUT - new_w) // 2, (INPUT - new_h) // 2
    canvas = np.full((INPUT, INPUT, 3), 114, dtype=np.uint8)
    canvas[pad_y:pad_y + new_h, pad_x:pad_x + new_w] = resized
    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    chw = rgb.transpose(2, 0, 1)
    return chw[np.newaxis], (scale, pad_x, pad_y, w, h)


def postprocess(out, params, conf, iou):
    scale, pad_x, pad_y, w, h = params
    pred = out[0]  # [5, 8400]
    scores = pred[4]
    keep = scores > conf
    boxes = pred[:4, keep].T  # xywh
    scs = scores[keep]
    if len(scs) == 0:
        return []
    # NMS (xywh)
    x, y = boxes[:, 0], boxes[:, 1]
    x1, y1, x2, y2 = x - boxes[:, 2] / 2, y - boxes[:, 3] / 2, x + boxes[:, 2] / 2, y + boxes[:, 3] / 2
    areas = boxes[:, 2] * boxes[:, 3]
    order = scs.argsort()[::-1]
    keep_idx = []
    while order.size:
        i = order[0]
        keep_idx.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        inter = np.maximum(0, xx2 - xx1) * np.maximum(0, yy2 - yy1)
        iou_v = inter / (areas[i] + areas[order[1:]] - inter + 1e-9)
        order = order[1:][iou_v <= iou]
    dets = []
    for i in keep_idx:
        cx, cy, bw, bh = boxes[i]
        # 反 letterbox: 模型坐标 -> 原图坐标 (x-pad)/scale
        dets.append((
            (cx - pad_x) / scale, (cy - pad_y) / scale,
            bw / scale, bh / scale, float(scs[i])))
    return dets


def main():
    sess = ort.InferenceSession(ONNX, providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name
    print(f"input: {name} {sess.get_inputs()[0].shape}  "
          f"output: {sess.get_outputs()[0].name} {sess.get_outputs()[0].shape}")

    imgs = sys.argv[1:] or ["../../captures/images/000.jpg"]
    for path in imgs:
        bgr = cv2.imread(path)
        if bgr is None:
            print(f"[SKIP] cannot read {path}")
            continue
        tensor, params = letterbox(bgr)
        out = sess.run(None, {name: tensor})[0]
        dets = postprocess(out, params, CONF, IOU)
        print(f"{path} ({bgr.shape[1]}x{bgr.shape[0]}): "
              f"{len(dets)} person(s)")
        for cx, cy, w, h, s in dets:
            print(f"  xywh=({cx:.0f},{cy:.0f},{w:.0f},{h:.0f}) conf={s:.2f}")

        if len(sys.argv) == 1:
            # 半身: 裁掉下 45% (只留头肩)
            half = bgr[: int(bgr.shape[0] * 0.55)]
            tensor, params = letterbox(half)
            out = sess.run(None, {name: tensor})[0]
            dets = postprocess(out, params, CONF, IOU)
            print(f"half-body crop ({half.shape[1]}x{half.shape[0]}): "
                  f"{len(dets)} person(s)")
            for cx, cy, w, h, s in dets:
                print(f"  xywh=({cx:.0f},{cy:.0f},{w:.0f},{h:.0f}) conf={s:.2f}")


if __name__ == "__main__":
    main()
