#!/usr/bin/env python3
"""只清理本项目生成的缓存与上一次打包产物，不碰源码、实测数据和个人文件。"""
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
n = 0
for p in list(ROOT.rglob("__pycache__")) + list(ROOT.rglob("*.pyc")):
    if ".workbuddy" in p.parts or "dist" in p.parts:
        continue
    try:
        shutil.rmtree(p) if p.is_dir() else p.unlink()
        n += 1
    except OSError:
        pass
for rel in ("dist/course-kit",):
    p = ROOT / rel
    if p.is_dir():
        shutil.rmtree(p)
        n += 1
for p in (ROOT / "dist").glob("heterogeneous-course-kit-*.tar.gz"):
    p.unlink(); n += 1
for p in (ROOT / "dist").glob("heterogeneous-course-kit-*.zip"):
    p.unlink(); n += 1
print("已清理 %d 项缓存/生成物" % n)
