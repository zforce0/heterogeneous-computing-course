#!/usr/bin/env python3
"""把上游资产（PPT/教具/手册/测评/教案/实机平台）同步进 V6.4 产品包。

只复制、不改源文件。排除 .DS_Store、__pycache__、.slidep 等本机/缓存文件；
教具包与测评包的「示例结果包」保留，作为教师视角的预期输出样例。
"""
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
V64 = HERE.parent
COURSE = V64.parent / "课程材料"                      # 课程材料（上游）
PLATFORM = V64.parent.parent / "异构计算软件工程_Software" / "dist" / "course-kit"  # 实机平台（上游）

IGNORE_NAMES = {".DS_Store", "__pycache__", ".slidep", ".git", ".pytest_cache", ".venv"}


def _ignore(cur, names):
    return [n for n in names if n in IGNORE_NAMES]


def copy_tree(src, dst, ignore_names=True):
    if not src.exists():
        print("  [SKIP] 不存在: %s" % src)
        return 0
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, ignore=_ignore if ignore_names else None)
    return 1


def main():
    print("== 同步上游资产到 V6.4 ==")
    jobs = [
        (COURSE / "PPT", V64 / "02-理论课-PPT"),
        (COURSE / "教具包", V64 / "03-教具包"),
        (COURSE / "实验手册", V64 / "04-实验手册"),
        (COURSE / "测评包", V64 / "05-测评包"),
    ]
    n = 0
    for src, dst in jobs:
        n += copy_tree(src, dst)
        print("  %s -> %s" % (src.name, dst.name))
    # 示范课教案（两个 docx）
    if not (V64 / "09-示范课教案").exists():
        (V64 / "09-示范课教案").mkdir(parents=True)
    for f in COURSE.glob("示范课教案-*.docx"):
        shutil.copy2(f, V64 / "09-示范课教案" / f.name)
        n += 1
        print("  %s -> 09-示范课教案/" % f.name)
    # 实机平台
    n += copy_tree(PLATFORM, V64 / "06-实验平台")
    print("  course-kit -> 06-实验平台")
    print("完成，复制 %d 项" % n)


if __name__ == "__main__":
    main()
