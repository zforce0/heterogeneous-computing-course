#!/usr/bin/env python3
"""验收课程包（不改原工程）：校验清单、脱敏、语法，并在临时目录模拟解包重建页面。

    python3 scripts/verify_package.py
"""
import ast
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sanitize  # noqa: E402

STAGE = ROOT / "dist" / "course-kit"
FAILED = []


def check(ok, msg):
    print(("[OK]   " if ok else "[FAIL] ") + msg)
    if not ok:
        FAILED.append(msg)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def main():
    check(STAGE.is_dir(), "课程包目录存在")
    if not STAGE.is_dir():
        return 1
    mf = STAGE / "MANIFEST.json"
    check(mf.is_file(), "MANIFEST.json 存在")
    if not mf.is_file():
        return 1
    manifest = json.loads(mf.read_text(encoding="utf-8"))

    bad_hash = []
    missing = []
    for ent in manifest["files"]:
        p = STAGE / ent["path"]
        if not p.is_file():
            missing.append(ent["path"])
        elif sha256(p) != ent["sha256"]:
            bad_hash.append(ent["path"])
    check(not missing, "文件齐全（缺失 %d）" % len(missing))
    check(not bad_hash, "SHA256 一致（不符 %d）" % len(bad_hash))
    check(not sanitize.find_leftovers(STAGE), "无残留明文设备口令")

    required = ["README.md", "docs/05-course-map.md", "docs/02-deployment.md",
                "config/lab.env.example", "eval_platform/dashboard/serve_g4.py",
                "eval_platform/tools/node_roster.py", "ARCHIVE_NOTICE.md"]
    for rel in required:
        check((STAGE / rel).is_file(), "关键文件 " + rel)

    binaries = [p for p in STAGE.rglob("*") if p.is_file() and
                p.suffix.lower() in {".rknn", ".onnx", ".pt", ".gguf", ".bin"}]
    check(not binaries, "未混入大二进制模型（%d）" % len(binaries))

    pys = list((STAGE / "scripts").glob("*.py")) + \
          list((STAGE / "eval_platform/dashboard").glob("*.py")) + \
          list((STAGE / "eval_platform/config").glob("*.py"))
    bad_py = []
    for p in pys:
        try:
            ast.parse(p.read_text(encoding="utf-8"))
        except Exception:
            bad_py.append(p.name)
    check(not bad_py, "Python 语法（失败：%s）" % (", ".join(bad_py) or "无"))

    shells = list((STAGE / "scripts").glob("*.sh")) + \
             list((STAGE / "eval_platform/tools").glob("*.sh"))
    bad_sh = []
    for p in shells:
        r = subprocess.run(["bash", "-n", str(p)], capture_output=True)
        if r.returncode:
            bad_sh.append(p.name)
    check(not bad_sh, "Shell 语法（失败：%s）" % (", ".join(bad_sh) or "无"))

    print("\n== 在临时目录模拟部署 ==")
    with tempfile.TemporaryDirectory(prefix="g4-kit-") as td:
        dst = Path(td) / "course-kit"
        shutil.copytree(STAGE, dst)
        r = subprocess.run([sys.executable, "scripts/doctor.py"], cwd=str(dst),
                           capture_output=True, text=True)
        check(r.returncode == 0, "doctor.py 在干净副本里通过")
        if r.returncode:
            print(r.stdout[-1200:])
        r = subprocess.run([sys.executable, "eval_platform/dashboard/build_g4_dashboard.py"],
                           cwd=str(dst), capture_output=True, text=True)
        check(r.returncode == 0, "离线重建 g4_live.html 通过")
        if r.returncode:
            print(r.stdout[-1200:], r.stderr[-600:])
        html = dst / "eval_platform/dashboard/g4_live.html"
        check(html.is_file() and html.stat().st_size > 10000, "重建后的页面非空")
        r = subprocess.run([sys.executable, "-c",
                            "import sys; sys.path.insert(0,'eval_platform/config');"
                            "import lab_config as l; print(l.endpoint('ascend','llm'))"],
                           cwd=str(dst), capture_output=True, text=True)
        check(r.returncode == 0 and "8080" in r.stdout, "可移植配置模块可导入")

    print("\n结论：%s" % ("全部通过" if not FAILED else "%d 项失败" % len(FAILED)))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
