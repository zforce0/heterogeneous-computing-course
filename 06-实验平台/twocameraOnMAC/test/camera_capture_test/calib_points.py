#!/usr/bin/env python3
"""calib_points.py — 交互式双摄配准选点标定工具

用法 (需桌面 GUI):
  cd twocameraOnMAC
  <YOLOFuse venv python> test/camera_capture_test/calib_points.py [图像编号]

操作:
  1. 两个窗口分别显示可见光(RGB)和红外(IR)同一时刻的标定图
  2. 先在 RGB 窗口点击一个特征点 (角/边缘/物体轮廓),
     再在 IR 窗口点击同一物理位置 → 完成一对对应点
  3. 重复标 4~8 对 (分布越均匀越好)
  4. 按 c 计算; 右键或 u 撤销最后一对; n 下一对图; q 退出

实现说明: 鼠标回调内只记录坐标, 绝不调用 imshow (GTK 后端下
回调内绘图会导致事件分发失效), 重绘统一在主循环完成。

输出:
  - IR 视野在 RGB 图中的框 (bbox)
  - 建议的 --crop-ratio 参数与 sample_config.h 宏值
"""
import cv2
import numpy as np
import glob
import os
import sys

os.chdir(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))  # 项目根目录

pairs = sorted(glob.glob("captures/images/*.jpg"))
if not pairs:
    print("no captures found under captures/images")
    sys.exit(1)

start = int(sys.argv[1]) if len(sys.argv) > 1 else 0
index = min(max(start, 0), len(pairs) - 1)

rgb_pts = []
ir_pts = []
rgb_base = None   # 原图 (含标定结果框)
ir_base = None
result_box = None  # (x, y, w, h) 已计算的 IR 视野框
win_rgb, win_ir = "RGB - click point", "IR - click same point"
IR_VIEW_W, IR_VIEW_H = 512, 384   # IR 放大显示尺寸


def load(i):
    global rgb_base, ir_base, rgb_pts, ir_pts, result_box
    p = pairs[i]
    rgb_base = cv2.imread(p)
    ir_orig = cv2.imread(p.replace("images/", "imagesIR/"))
    if rgb_base is None or ir_orig is None:
        print("load failed:", p)
        return False
    ir_base = cv2.resize(ir_orig, (IR_VIEW_W, IR_VIEW_H))
    rgb_pts, ir_pts = [], []
    result_box = None
    return True


def undo():
    global result_box
    if len(rgb_pts) > len(ir_pts):
        rgb_pts.pop()
    elif ir_pts:
        ir_pts.pop()
    result_box = None


def on_rgb(event, x, y, flags, param):
    # 只记录坐标, 不做任何绘图
    if event == cv2.EVENT_LBUTTONDOWN:
        if len(rgb_pts) == len(ir_pts):
            rgb_pts.append((x, y))
    elif event == cv2.EVENT_RBUTTONDOWN:
        undo()


def on_ir(event, x, y, flags, param):
    if event == cv2.EVENT_LBUTTONDOWN:
        if len(rgb_pts) == len(ir_pts) + 1:
            # 还原到原始 IR 分辨率坐标 (256x192)
            ir_pts.append((int(round(x * 256 / IR_VIEW_W)),
                           int(round(y * 192 / IR_VIEW_H))))
    elif event == cv2.EVENT_RBUTTONDOWN:
        undo()


def redraw():
    r = rgb_base.copy()
    ir = ir_base.copy()
    paired = min(len(rgb_pts), len(ir_pts))
    for i, pt in enumerate(rgb_pts):
        done = i < paired
        color = (0, 200, 0) if done else (0, 0, 255)
        cv2.circle(r, pt, 4, color, -1)
        cv2.putText(r, str(i), (pt[0] + 6, pt[1] - 6),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)
    for i, pt in enumerate(ir_pts):
        color = (0, 200, 0) if i < paired else (0, 0, 255)
        px = int(pt[0] * IR_VIEW_W / 256)
        py = int(pt[1] * IR_VIEW_H / 192)
        cv2.circle(ir, (px, py), 4, color, -1)
        cv2.putText(ir, str(i), (px + 6, py - 6),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)

    # 状态提示
    if len(rgb_pts) == len(ir_pts):
        hint = f"pairs:{paired}  -> click next point on RGB"
    else:
        hint = f"pairs:{paired}+1  -> click SAME point on IR"
    if result_box is not None:
        x, y, w, h = result_box
        cv2.rectangle(r, (x, y), (x + w, y + h), (0, 255, 0), 2)
        hint += "  [green box = IR FOV]"
    cv2.putText(r, hint + "  [c]calc [right-click/u]undo [n]next [q]quit",
                (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                (0, 255, 255), 1, cv2.LINE_AA)
    cv2.imshow(win_rgb, r)
    cv2.imshow(win_ir, ir)


def compute():
    global result_box
    n = min(len(rgb_pts), len(ir_pts))
    if n < 4:
        print("need >= 4 point pairs, now paired:", n)
        return
    src = np.float32(ir_pts[:n]).reshape(-1, 1, 2)
    dst = np.float32(rgb_pts[:n]).reshape(-1, 1, 2)
    H, mask = cv2.findHomography(src, dst, cv2.RANSAC, 3.0)
    if H is None:
        print("homography failed, check point pairs")
        return
    inliers = 0 if mask is None else int(mask.sum())
    if inliers < 4:
        print("too few RANSAC inliers:", inliers,
              "- check wrong pairs (right-click to undo)")
        return

    h, w = rgb_base.shape[:2]
    corners = np.float32([[0, 0], [256, 0], [256, 192],
                          [0, 192]]).reshape(-1, 1, 2)
    mapped = cv2.perspectiveTransform(corners, H).reshape(-1, 2)
    x0, y0 = mapped.min(axis=0)
    x1, y1 = mapped.max(axis=0)
    # 规范为 4:3 并 clamp 到图内
    cw, ch = x1 - x0, y1 - y0
    if cw <= 0 or ch <= 0:
        print("degenerate box, check point pairs")
        return
    if cw / ch > 4 / 3:
        cw = ch * 4 / 3
    else:
        ch = cw * 3 / 4
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    crop_w, crop_h = int(round(cw)), int(round(ch))
    crop_x = max(0, min(int(round(cx - crop_w / 2)), w - crop_w))
    crop_y = max(0, min(int(round(cy - crop_h / 2)), h - crop_h))
    result_box = (crop_x, crop_y, crop_w, crop_h)
    ratio = crop_w / w

    print("\n===== calibration result =====")
    print(f"RGB size: {w}x{h}, point pairs: {n}, RANSAC inliers: {inliers}")
    print(f"IR-FOV crop box: x={crop_x} y={crop_y} w={crop_w} h={crop_h}")
    print(f"crop ratio (w / visible_w): {ratio:.3f}")
    print(f"\nrun with:      ./build/run.sh --crop-ratio {ratio:.3f}")
    print(f"or edit sample_config.h:  #define REGISTRATION_CROP_RATIO {ratio:.3f}f")
    cv2.rectangle(rgb_base, (crop_x, crop_y),
                  (crop_x + crop_w, crop_y + crop_h), (0, 255, 0), 2)
    cv2.imwrite("/tmp/calib_result_box.jpg", rgb_base)
    print("verify image: /tmp/calib_result_box.jpg (green box = IR FOV)")


cv2.namedWindow(win_rgb, cv2.WINDOW_NORMAL)
cv2.namedWindow(win_ir, cv2.WINDOW_NORMAL)
cv2.setMouseCallback(win_rgb, on_rgb)
cv2.setMouseCallback(win_ir, on_ir)

if not load(index):
    sys.exit(1)
print(f"image pair {index}: {pairs[index]}")
print("click a feature on RGB window, then the SAME feature on IR window;"
      " collect >= 4 pairs, then press 'c'")

while True:
    redraw()  # 主循环统一重绘 (不在鼠标回调内)
    key = cv2.waitKey(40) & 0xFF
    if key == ord('q') or key == 27:
        break
    elif key == ord('c'):
        compute()
    elif key == ord('u'):
        undo()
    elif key == ord('n'):
        index = (index + 1) % len(pairs)
        if load(index):
            print(f"image pair {index}: {pairs[index]}")

cv2.destroyAllWindows()
