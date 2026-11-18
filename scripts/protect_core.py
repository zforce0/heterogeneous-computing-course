#!/usr/bin/env python3
"""核心课程材料保护：单密码解锁全部 + 明文目录清单可预览。

用法:
    KIT_PASSWORD='<密码>' python3 scripts/protect_core.py encrypt
    KIT_PASSWORD='<密码>' python3 scripts/protect_core.py decrypt

设计:
  * 密码只从环境变量 KIT_PASSWORD 读取，绝不写入仓库；
  * 5 类核心目录先 tar + gzip 压缩，再合成**一个** protected/course-core.tar.enc
    （AES-256-CBC，PBKDF2 200000 轮 + 随机 salt）——输入一次密码，全部解锁。
    压缩后体积约为原来的 1/3，浏览器/Pages 下载不再长时间卡住；
    解密端自动识别「是否 gzip」以兼容旧格式包；
  * 原目录只保留明文「目录清单.md」：列出每个文件名与大小，方便预览结构，
    但不含任何文件内容（PPT/手册/测评/教案正文不可见）；
  * 购课学校拿到密码后运行 decrypt，一次还原全部 4 个目录。
"""
import gzip
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTECTED = ["02-理论课-PPT", "03-教具包", "04-实验手册", "05-测评包", "09-示范课教案"]
# 这些目录在锁定状态下保留明文 README（可预览设计，不含细节）
KEEP_README = {"03-教具包"}
OUT = ROOT / "protected"
ENC = OUT / "course-core.tar.enc"


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        sys.exit("命令失败: %s\n%s" % (" ".join(cmd), r.stderr.decode("utf-8", "replace")))
    return r


def open_tar(path):
    """自动识别 gzip 压缩或裸 tar，兼容压缩前后的两种加密包格式。"""
    return tarfile.open(path, "r:*")


def preview_listing(name, src):
    """生成明文目录清单：文件名 + 大小，无内容。"""
    lines = ["# %s · 目录清单（预览）" % name, "",
             "> 本目录为核心课程材料，内容已加密。此处仅列出文件结构供预览，",
             "> 不含任何文件内容。解锁后清单会被原始文件替换。", "",
             "解锁：`KIT_PASSWORD='<授权密码>' python3 scripts/protect_core.py decrypt`", "",
             "## 文件列表", "", "| 路径 | 大小 |", "|---|---:|"]
    files = sorted(p for p in src.rglob("*") if p.is_file())
    for p in files:
        rel = p.relative_to(src).as_posix()
        kb = p.stat().st_size / 1024
        size = "%.0f B" % p.stat().st_size if kb < 1 else "%.0f KB" % kb
        lines.append("| `%s` | %s |" % (rel, size))
    lines += ["", "共 **%d** 个文件。" % len(files), ""]
    return "\n".join(lines)


def encrypt():
    if not OUT.exists():
        OUT.mkdir(parents=True)
    missing = [n for n in PROTECTED if not (ROOT / n).is_dir()]
    if missing:
        sys.exit("缺少待加密目录: %s" % ", ".join(missing))

    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tf:
        tar_path = Path(tf.name)
    gz_path = tar_path.with_suffix(".gz")
    with tarfile.open(tar_path, "w") as tar:
        for name in PROTECTED:
            tar.add(ROOT / name, arcname=name)
    # 先 gzip 压缩（内置模块，无第三方依赖），显著降低下载体积
    with open(tar_path, "rb") as fin, gzip.open(gz_path, "wb", compresslevel=9) as fout:
        shutil.copyfileobj(fin, fout)

    run(["openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
         "-salt", "-in", str(gz_path), "-out", str(ENC), "-pass", "env:KIT_PASSWORD"])
    tar_path.unlink()
    gz_path.unlink()

    # 校验：能解密、能解压且各目录齐全
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tf:
        chk = Path(tf.name)
    run(["openssl", "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
         "-in", str(ENC), "-out", str(chk), "-pass", "env:KIT_PASSWORD"])
    with open_tar(chk) as t:  # gzip 或裸 tar 均可识别
        names = t.getnames()
    chk.unlink()
    for name in PROTECTED:
        assert any(n.startswith(name + "/") for n in names), name

    # 明文目录 → 目录清单桩
    for name in PROTECTED:
        src = ROOT / name
        listing = preview_listing(name, src)
        keep = (src / "README.md").read_bytes() if name in KEEP_README and (src / "README.md").is_file() else None
        shutil.rmtree(src)
        src.mkdir()
        if keep is not None:
            (src / "README.md").write_bytes(keep)
        (src / "目录清单.md").write_text(listing, encoding="utf-8")
        print("[OK] %-16s -> 明文清单 %d 文件%s" % (name, listing.count("| `"),
              " + README" if keep is not None else ""))
    # 删旧的分包（若存在）
    for name in PROTECTED:
        old = OUT / (name + ".tar.enc")
        if old.exists():
            old.unlink()
    print("[OK] 单包 -> protected/course-core.tar.enc (%.0f KB, 一次密码解锁全部)" %
          (ENC.stat().st_size / 1024))


def decrypt():
    if not ENC.is_file():
        sys.exit("未找到加密包: %s" % ENC)
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tf:
        tmp = Path(tf.name)
    run(["openssl", "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
         "-in", str(ENC), "-out", str(tmp), "-pass", "env:KIT_PASSWORD"])
    with tempfile.TemporaryDirectory() as td:
        with open_tar(tmp) as t:  # 兼容 gzip 压缩的新格式与裸 tar 旧格式
            t.extractall(td, filter="data")
        for name in PROTECTED:
            src = Path(td) / name
            if not src.is_dir():
                sys.exit("加密包缺少目录: %s" % name)
            dst = ROOT / name
            shutil.rmtree(dst, ignore_errors=True)
            shutil.move(str(src), str(dst))
            n = sum(1 for p in dst.rglob("*") if p.is_file())
            print("[OK] 已还原 %-16s (%d 文件)" % (name, n))
    tmp.unlink()


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode not in ("encrypt", "decrypt"):
        sys.exit(__doc__)
    if "KIT_PASSWORD" not in os.environ:
        sys.exit("请先设置环境变量 KIT_PASSWORD（密码不写入仓库）")
    (encrypt if mode == "encrypt" else decrypt)()
