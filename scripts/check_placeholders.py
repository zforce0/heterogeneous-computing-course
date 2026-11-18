#!/usr/bin/env python3
"""占位符检查：找出产品包中尚未替换为真实学校信息的占位符。

    python3 scripts/check_placeholders.py

脱敏通用包中的校名/报价等使用 `<占位符>` 占位；交付某校前应全部替换。
本脚本扫描规划/展示/商务三处 Markdown，报告残留占位符及所在文件，退出码：
  0 = 无残留（可交付）
  1 = 有残留（需按《学校适配指南》替换）
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCAN = ["01-课程规划", "07-展示材料", "08-商务材料", "README.md"]
PAT = re.compile(r"<[A-Za-z\u4e00-\u9fff][^>\n]{0,24}>")


def main():
    hits = {}
    for rel in SCAN:
        p = ROOT / rel
        files = [p] if p.is_file() else sorted(p.glob("*.md"))
        for f in files:
            for i, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
                for m in PAT.findall(line):
                    hits.setdefault(m, []).append("%s:%d" % (f.relative_to(ROOT), i))
    if not hits:
        print("无残留占位符，可交付。")
        return 0
    print("发现 %d 类占位符待替换：" % len(hits))
    for ph in sorted(hits):
        print("  %-16s %s" % (ph, ", ".join(hits[ph][:4])))
    print("\n按《01-课程规划/学校适配指南.md》替换后重跑本脚本。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
