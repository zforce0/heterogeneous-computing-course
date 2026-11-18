#!/usr/bin/env python3
"""核心课程材料加密（AES-256-CBC，openssl）。

用法:
    KIT_PASSWORD='<密码>' python3 scripts/protect_core.py encrypt   # 生成受保护包
    KIT_PASSWORD='<密码>' python3 scripts/protect_core.py decrypt   # 解包还原

加密对象（课程核心成品，仅 demo 明文）:
    02-理论课-PPT / 04-实验手册 / 05-测评包 / 09-示范课教案

设计:
  * 密码只从环境变量 KIT_PASSWORD 读取，绝不写入仓库；
  * 每个目录 → tar → openssl enc -aes-256-cbc（PBKDF2 200000 轮 + 随机 salt）→ <名称>.tar.enc；
  * 明文目录从仓库移除，只保留加密包 + 明文桩 README（说明如何解锁）；
  * 交付时把密码线下发给购课学校，对方运行 decrypt 即可还原。
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTECTED = ["02-理论课-PPT", "04-实验手册", "05-测评包", "09-示范课教案"]
OUT = ROOT / "protected"


def pw():
    p = os.environ.get("KIT_PASSWORD")
    if not p:
        sys.exit("请先设置环境变量 KIT_PASSWORD（密码不写入仓库）")
    # 密码只从环境变量读取；openssl -salt 自动随机 salt 写入密文头，无需单独保存
    return p


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode:
        sys.exit("命令失败: %s\n%s" % (" ".join(cmd), r.stderr.decode("utf-8", "replace")))
    return r


def encrypt():
    OUT.mkdir(exist_ok=True)
    for name in PROTECTED:
        src = ROOT / name
        if not src.is_dir():
            print("跳过（不存在）:", name)
            continue
        with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tf:
            tar_path = Path(tf.name)
        with tarfile.open(tar_path, "w") as tar:
            tar.add(src, arcname=name)
        enc = OUT / (name + ".tar.enc")
        run(["openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
             "-salt", "-in", str(tar_path), "-out", str(enc), "-pass",
             "env:KIT_PASSWORD"])
        tar_path.unlink()
        # 校验：能解密且 tar 可列
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
            chk = Path(tf.name)
        run(["openssl", "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
             "-in", str(enc), "-out", str(chk), "-pass", "env:KIT_PASSWORD"])
        with tarfile.open(chk) as t:
            n = len(t.getnames())
        chk.unlink()
        print("[OK] %-16s -> protected/%s.tar.enc (%d 文件, %.0f KB)" % (
            name, name, n, enc.stat().st_size / 1024))
        # 移除明文目录，留桩 README
        shutil.rmtree(src)
        src.mkdir()
        (src / "README.md").write_text(
            "# %s（核心材料，已加密）\n\n"
            "本目录内容为课程核心材料，已加密为 `protected/%s.tar.enc`。\n\n"
            "## 解锁（购课学校）\n\n"
            "```bash\n"
            "KIT_PASSWORD='<授权密码>' python3 scripts/protect_core.py decrypt\n"
            "```\n\n"
            "密码由课程授权方线下提供，不随课程包分发。详见仓库根目录 `README.md`。\n"
            % (name, name), encoding="utf-8")


def decrypt():
    for name in PROTECTED:
        enc = OUT / (name + ".tar.enc")
        if not enc.is_file():
            print("跳过（未找到加密包）:", name)
            continue
        with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tf:
            tmp = Path(tf.name)
        run(["openssl", "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
             "-in", str(enc), "-out", str(tmp), "-pass", "env:KIT_PASSWORD"])
        # 解到临时目录再覆盖，避免残留桩文件
        with tempfile.TemporaryDirectory() as td:
            with tarfile.open(tmp) as t:
                t.extractall(td, filter="data")
            dst = ROOT / name
            shutil.rmtree(dst, ignore_errors=True)
            shutil.move(str(Path(td) / name), str(dst))
        tmp.unlink()
        print("[OK] 已还原:", name)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode not in ("encrypt", "decrypt"):
        sys.exit(__doc__)
    (encrypt if mode == "encrypt" else decrypt)()
