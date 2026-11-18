#!/usr/bin/env python3
"""Rotate every thermal (IR) frame in the workspace 90 degrees counter-clockwise.

macOS `sips -r 270` == 90 deg CCW (verified with a synthetic marker: top-left
moves to bottom-left and the dimensions swap). Rotation is physical (no EXIF tag).

Also refreshes derived artifacts that embed the IR images:
  - eval_platform/dashboard/data.json  -> detection.pairs[].ir
  - runs/.../keyframes.html            -> embedded IR base64 + footnote
  - runs/.../keyframes.json            -> rotation metadata
A full byte-for-byte backup is written before anything is modified.
Re-running is safe: files already in portrait orientation are skipped.
"""
import base64
import datetime
import json
import os
import re
import shutil
import subprocess
import zoneinfo
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SAMPLES = ROOT / "eval_platform/dashboard/samples"
CAPTURES_IR = ROOT / "twocameraOnMAC/captures/imagesIR"
DATA_JSON = ROOT / "eval_platform/dashboard/data.json"
RUN = ROOT / "eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous"
KEY_HTML = RUN / "keyframes.html"
KEY_JSON = RUN / "keyframes.json"
BACKUP = ROOT / ".workbuddy/backups/ir_rotation_20261113"
RECORD = ROOT / "eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/ir_rotation.json"
TZ = zoneinfo.ZoneInfo("Asia/Shanghai")


def ir_files():
    files = []
    files += sorted(SAMPLES.glob("det_*_ir.jpg"))
    files += sorted(SAMPLES.glob("sm_det_*_ir.jpg"))
    files += sorted(CAPTURES_IR.glob("*.jpg"))
    return files


def dims(path):
    out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", str(path)],
                         capture_output=True, text=True).stdout
    w = re.search(r"pixelWidth:\s*(\d+)", out)
    h = re.search(r"pixelHeight:\s*(\d+)", out)
    return (int(w.group(1)), int(h.group(1))) if w and h else (None, None)


def main():
    files = ir_files()
    print(f"发现热成像文件 {len(files)} 张")
    original_bytes = {p: p.read_bytes() for p in files}
    BACKUP.mkdir(parents=True, exist_ok=True)

    rotated, skipped = [], []
    for p in files:
        w, h = dims(p)
        if w and h and w < h:
            print(f"  SKIP 已是竖向（疑似已旋转）: {p.relative_to(ROOT)} {w}x{h}")
            skipped.append(str(p.relative_to(ROOT)))
            continue
        dest = BACKUP / p.relative_to(ROOT)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dest)
        tmp = p.with_name(p.name + ".rot.tmp.jpg")
        r = subprocess.run(["sips", "-r", "270", "-s", "formatOptions", "95",
                            str(p), "--out", str(tmp)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not tmp.exists():
            raise RuntimeError(f"sips 旋转失败 {p}: {r.stderr.strip()}")
        os.replace(tmp, p)
        nw, nh = dims(p)
        if (nw, nh) != (h, w):
            raise RuntimeError(f"尺寸校验失败 {p}: {(w, h)} -> {(nw, nh)}")
        rotated.append({"path": str(p.relative_to(ROOT)),
                        "before": f"{w}x{h}", "after": f"{nw}x{nh}",
                        "backup": str(dest.relative_to(ROOT))})
        print(f"  OK   {p.relative_to(ROOT)}  {w}x{h} -> {nw}x{nh}")

    new_bytes = {p: p.read_bytes() for p in files}
    replaced = {"data_json": 0, "keyframes_html": 0}

    # 1) dashboard data.json: detection.pairs[].ir == sm_det_*_ir.jpg
    text = DATA_JSON.read_text()
    for name in ("889", "968", "064", "158"):
        matches = list(SAMPLES.glob(f"sm_det_*_{name}_ir.jpg"))
        if not matches:
            continue
        p = matches[0]
        old = base64.b64encode(original_bytes[p]).decode()
        new = base64.b64encode(new_bytes[p]).decode()
        if old in text:
            text = text.replace(old, new, 1)
            replaced["data_json"] += 1
    DATA_JSON.write_text(text)

    # 2) keyframes.html: embedded det_*_ir.jpg base64 + footnote
    html = KEY_HTML.read_text()
    for p in sorted(SAMPLES.glob("det_*_ir.jpg")):
        old = base64.b64encode(original_bytes[p]).decode()
        new = base64.b64encode(new_bytes[p]).decode()
        if old in html:
            html = html.replace(old, new, 1)
            replaced["keyframes_html"] += 1
    footnote_old = "热成像为相对辐射强度显示；未取得标定协议前"
    footnote_new = ("所有热成像帧已于 2026-11-13 统一<b>逆时针旋转 90°</b>"
                    "（256×192 → 192×256；缩略图 480×360 → 360×480），检测框随图同步旋转；"
                    "热成像为相对辐射强度显示；未取得标定协议前")
    if footnote_old in html and "逆时针旋转 90°" not in html:
        html = html.replace(footnote_old, footnote_new, 1)
    KEY_HTML.write_text(html)

    # 3) keyframes.json metadata
    kj = json.loads(KEY_JSON.read_text())
    kj["ir_rotation"] = {
        "applied_local": datetime.datetime.now(TZ).strftime("%Y-%m-%d %H:%M:%S %z"),
        "direction": "counter-clockwise 90 deg (sips -r 270)",
        "files_rotated": len(rotated),
        "files_skipped": len(skipped),
        "backup_dir": str(BACKUP.relative_to(ROOT)),
        "note": "热成像原图与内嵌副本已同步旋转；可见光帧与六宫格 viz 合成图未改动",
    }
    KEY_JSON.write_text(json.dumps(kj, ensure_ascii=False, indent=2) + "\n")

    rec = {
        "schema_version": "ir-rotation-v1",
        "applied_local": datetime.datetime.now(TZ).strftime("%Y-%m-%d %H:%M:%S %z"),
        "direction": "逆时针 90° (CCW), sips -r 270, physical pixel rotation",
        "jpeg_quality": 95,
        "backup_dir": str(BACKUP.relative_to(ROOT)),
        "restore_hint": "cp -R <backup_dir>/<相对路径> <原路径>",
        "rotated": rotated,
        "skipped": skipped,
        "derived_updates": replaced,
        "unchanged": [
            "可见光帧 det_*_vis.jpg / sm_det_*_vis.jpg / twocameraOnMAC/captures/images/*.jpg",
            "六宫格合成图 viz_*.jpg / sm_viz_*.jpg（内含热成像面板，未单独旋转）",
            "eval_platform/dashboard/camera_stream.py 实时预览：原本已做 CCW 90，无需改动",
        ],
    }
    RECORD.write_text(json.dumps(rec, ensure_ascii=False, indent=2) + "\n")
    print(f"\n备份目录 : {BACKUP.relative_to(ROOT)}")
    print(f"旋转 {len(rotated)} 张，跳过 {len(skipped)} 张")
    print(f"派生产物更新: data.json 内嵌 IR {replaced['data_json']} 处, "
          f"keyframes.html 内嵌 IR {replaced['keyframes_html']} 处")
    print(f"记录: {RECORD.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
