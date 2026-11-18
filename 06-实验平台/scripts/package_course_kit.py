#!/usr/bin/env python3
"""生成可交付的《多核互联异构计算技术及应用》课程包（GitHub-ready）。

    python3 scripts/package_course_kit.py

产物（都在 dist/ 下，可整体删除后重生成）：
  dist/course-kit/                      干净目录，可直接作为 Git 仓库根目录上传
  dist/heterogeneous-course-kit-v1.0.tar.gz
  dist/heterogeneous-course-kit-v1.0.zip

原则：
  * 只收源码/脚本/文档/小规模实测快照；大二进制模型、编译产物、嵌套 .git 不进包。
  * 历史实验记录里的设备口令由 scripts/sanitize.py 脱敏；原始工程文件不动。
  * IP 作为归档事实保留，运行期代码已从 config/lab.env 读取（见 lab_config.py）。
"""
import datetime
import hashlib
import json
import os
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import sanitize  # noqa: E402

DIST = ROOT / "dist"
STAGE = DIST / "course-kit"
VERSION = "v1.0"
NAME = "heterogeneous-course-kit-" + VERSION

# (源相对路径, 目标相对路径, 是否递归)
FILES = [
    ("README.md", "README.md", False),
    ("Makefile", "Makefile", False),
    ("requirements.txt", "requirements.txt", False),
    ("requirements-ml.txt", "requirements-ml.txt", False),
    (".gitignore", ".gitignore", False),
    ("config/lab.env.example", "config/lab.env.example", False),
    ("互联异构计算实验设置_V1.2.md", "互联异构计算实验设置_V1.2.md", False),
    ("互联异构计算实验设置_V1.3.md", "互联异构计算实验设置_V1.3.md", False),
    ("Benchmark实验设置手册_V1.1（实机版）.docx", "Benchmark实验设置手册_V1.1（实机版）.docx", False),
    ("TC1-C非制冷红外热成像模组产品说明书-V1.7-20240617(1).pdf",
     "TC1-C非制冷红外热成像模组产品说明书-V1.7-20240617(1).pdf", False),
    ("性能评估测试平台方案/RK3588与RV1126B性能评估测试平台配置与实验规划_V1.0.docx",
     "性能评估测试平台方案/RK3588与RV1126B性能评估测试平台配置与实验规划_V1.0.docx", False),
    ("ascend_probe.txt", "ascend_probe.txt", False),
]

DIRS = [
    ("docs", "docs"),
    ("scripts", "scripts"),
    ("eval_platform/dashboard", "eval_platform/dashboard"),
    ("eval_platform/tools", "eval_platform/tools"),
    ("eval_platform/config", "eval_platform/config"),
    ("eval_platform/llm", "eval_platform/llm"),
    ("eval_platform/dashboard/samples", "eval_platform/dashboard/samples"),
    ("rv1126_test", "rv1126_test"),
    ("twocameraOnMAC", "twocameraOnMAC"),
    ("yolov8_detect_C_demo", "yolov8_detect_C_demo"),
]

# 归档记录只挑代表性报告，避免把 16 MB bundle.bin / 原始口令日志打进包
RUN = "eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/"
RUN_KEEP = ["report.md", "ascend_box_probe.md", "ascend_box_spec_report.md",
            "npu_attempt.md", "network_topology_snapshot.md", "keyframes_ascend.json",
            "keyframes.json", "manifest.json", "checksums.sha256"]
G3 = "eval_platform/runs/20261105T190201Z_G3_interconnect_heterogeneous/"
G3_KEEP = ["report.md", "summary.json", "manifest.json"]

IGNORE_NAMES = {".DS_Store", "__pycache__", ".git", ".pytest_cache", ".venv", "venv"}
IGNORE_SUFFIX = {".pyc", ".rknn", ".onnx", ".pt", ".pth", ".gguf", ".bin",
                 ".data", ".so", ".a", ".o", ".dylib", ".zip", ".tar", ".gz"}
IGNORE_DIR_PARTS = ("build", "captures", "output", "stage1", "stage2", "stage3",
                    "yolov8_detect_demo_release", "models/yolov8n")


def ignored_dir(rel):
    parts = rel.replace("\\", "/").split("/")
    return any(p in IGNORE_NAMES or p in IGNORE_DIR_PARTS for p in parts)


def copy_item(src_rel, dst_rel, recursive):
    src, dst = ROOT / src_rel, STAGE / dst_rel
    if not src.exists():
        print("  [SKIP] 不存在: %s" % src_rel)
        return 0
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not recursive:
        shutil.copy2(src, dst)
        return 1

    n = 0
    for path in src.rglob("*"):
        rel = path.relative_to(src).as_posix()
        # 历史日期日志含明文口令，且属个人过程记录，不进课程包
        if rel.startswith("2026") and rel.endswith(".md") and "twocameraOnMAC" in src_rel:
            continue
        if path.is_dir() and ignored_dir(rel):
            continue
        if not path.is_file():
            continue
        if path.name in IGNORE_NAMES or path.suffix.lower() in IGNORE_SUFFIX:
            continue
        if ignored_dir(rel):
            continue
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        n += 1
    return n


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_notice():
    (STAGE / "ARCHIVE_NOTICE.md").write_text("""# 打包说明（请先看）

这是《多核互联异构计算技术及应用》课程材料包的**源码与文档快照**。
打包时间、文件清单与校验值见 `MANIFEST.json`。

## 有意不包含的内容

| 类型 | 原因 | 如何取回 |
|---|---|---|
| `*.rknn` / `*.onnx` / `*.pt` / `*.gguf` | 体积大（合计 >100 MB）、许可各异 | 按 `docs/01-environment.md` 从板端或原实验机拷贝；模型文件名与输入形状已写入文档 |
| 编译产物 `build/`、`*.o`、可执行文件 | 平台相关（aarch64 / x86_64） | 用包内 `CMakeLists.txt` / `build.sh` / `build_edge_detectors.sh` 重建 |
| `twocameraOnMAC/2026*.md` 过程日志 | 含历史设备口令 | 留在原实验机，不上传 |
| `output/`、`.workbuddy/`、嵌套 `.git/` | 文档流水线中间产物与本机私有状态 | 不需要 |
| `config/lab.env` | 可能含本机地址/别名 | 用 `config/lab.env.example` 复制生成 |

## 安全

历史文档中的设备默认口令已由 `scripts/sanitize.py` 替换为 <REDACTED>；
SSH 私钥与口令从未写入本包。实验网 IP 是课程内网事实，保留在归档报告里；
**运行期代码不依赖这些常量**，改用 `config/lab.env`（见 `docs/01-environment.md`）。

## 诚实口径

归档报告中的实测数字来自原实验机（2026-10 ~ 2026-11），不是本包在新机器上重跑的结果。
换机部署后请以 `make doctor` 与页面 SSE 的实时输出为准；未测项标 `N/A`。
""", encoding="utf-8")


def write_manifest(count):
    files = []
    for p in sorted(STAGE.rglob("*")):
        if p.is_file() and p.name not in ("MANIFEST.json",):
            files.append({"path": p.relative_to(STAGE).as_posix(),
                          "bytes": p.stat().st_size,
                          "sha256": sha256(p)})
    manifest = {
        "name": NAME,
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "source": "互联异构计算课程材料平台（Mac 控制台 + 昇腾 LLM + RK3588 + RV1126B）",
        "copied_items": count,
        "files": files,
        "note": "归档实测数据来自原实验机；大二进制模型与过程日志有意排除，见 ARCHIVE_NOTICE.md",
    }
    (STAGE / "MANIFEST.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def make_archives():
    tar = DIST / (NAME + ".tar.gz")
    with tarfile.open(tar, "w:gz") as tf:
        tf.add(STAGE, arcname="course-kit")
    zpath = DIST / (NAME + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for p in sorted(STAGE.rglob("*")):
            if p.is_file():
                zf.write(p, "course-kit/" + p.relative_to(STAGE).as_posix())
    return tar, zpath


def main():
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True)
    if not DIST.exists():
        DIST.mkdir(parents=True)
    count = 0
    print("== 复制课程包内容 ==")
    for src, dst, rec in FILES:
        count += copy_item(src, dst, rec)
    for src, dst in DIRS:
        count += copy_item(src, dst, True)
    for fn in RUN_KEEP:
        count += copy_item(RUN + fn, "eval_platform/runs/" + RUN.split("/")[-2] + "/" + fn, False)
    for fn in G3_KEEP:
        count += copy_item(G3 + fn, "eval_platform/runs/" + G3.split("/")[-2] + "/" + fn, False)
    print("复制 %d 个文件" % count)

    print("== 脱敏 ==")
    changed = 0
    for p in STAGE.rglob("*"):
        changed += bool(sanitize.sanitize_file(p))
    print("改写 %d 个文本文件" % changed)
    leftovers = sanitize.find_leftovers(STAGE)
    if leftovers:
        print("!! 仍发现疑似口令，请人工核对: %s" % ", ".join(leftovers[:8]))
        return 2

    write_notice()
    manifest = write_manifest(count)
    tar, zpath = make_archives()
    print("== 完成 ==")
    print("目录: %s (%d 个文件)" % (STAGE, len(manifest["files"])))
    print("tar : %s (%.1f MB)" % (tar, tar.stat().st_size / 1e6))
    print("zip : %s (%.1f MB)" % (zpath, zpath.stat().st_size / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
