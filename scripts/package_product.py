#!/usr/bin/env python3
"""生成课程产品包交付物（tar.gz + zip + MANIFEST），供离线分发与归档。

    python3 scripts/package_product.py

产物：dist/heterogeneous-course-v6.4.{tar.gz,zip} + dist/MANIFEST.json
排除 .DS_Store / __pycache__ / .git / 学生 runs 输出 / 打包产物本身。
"""
import datetime
import hashlib
import json
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
VERSION = "v6.4"
NAME = "heterogeneous-course-" + VERSION
IGNORE_NAMES = {".DS_Store", "__pycache__", ".git", ".slidep", ".pytest_cache", ".venv"}


def sha256(p):
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def main():
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir(parents=True)

    files = []
    for p in sorted(ROOT.rglob("*")):
        if not p.is_file():
            continue
        if any(part in IGNORE_NAMES for part in p.relative_to(ROOT).parts):
            continue
        rel = p.relative_to(ROOT).as_posix()
        # 学生/运行产出与打包产物不进交付包
        if "/runs/" in rel or rel.startswith("dist/"):
            continue
        files.append(p)

    manifest = {
        "name": NAME,
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "files": [{"path": p.relative_to(ROOT).as_posix(),
                   "bytes": p.stat().st_size, "sha256": sha256(p)} for p in files],
        "note": "脱敏通用产品包；校名/报价为占位符，交付前按《学校适配指南》替换",
    }
    (DIST / "MANIFEST.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    tar = DIST / (NAME + ".tar.gz")
    with tarfile.open(tar, "w:gz") as tf:
        for p in files:
            tf.add(p, arcname="course/" + p.relative_to(ROOT).as_posix())
    zpath = DIST / (NAME + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for p in files:
            zf.write(p, "course/" + p.relative_to(ROOT).as_posix())

    print("打包 %d 个文件" % len(files))
    print("tar : %s (%.1f MB)" % (tar, tar.stat().st_size / 1e6))
    print("zip : %s (%.1f MB)" % (zpath, zpath.stat().st_size / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
