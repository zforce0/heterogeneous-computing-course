"""课程包文本脱敏：去掉历史实验记录里写死的板端口令。

只用于「导出课程包」这一步，不修改原始工程文件。
脱敏对象是设备默认/实验口令，不是实测数值；IP 与路径属于可公开的实验网事实，
保留在归档报告里，但运行期代码已改为从 config/lab.env 读取。
"""
import re
from pathlib import Path

TEXT_EXT = {".md", ".py", ".sh", ".yaml", ".yml", ".json", ".html", ".htm",
            ".txt", ".cpp", ".hpp", ".h", ".c", ".cmake", ".Dockerfile",
            ".service", ".example"}

# 顺序很重要：先长后短，避免先替掉短串导致长串漏网
RULES = [
    (re.compile(r"child\.sendline\(['\"](?:123456|980923)['\"]\)"),
     "child.sendline('<REDACTED>')"),
    (re.compile(r"(?i)(password|passwd|口令|密码)[^\d\n]{0,24}(?:123456|980923|temppwd|fish)"),
     r"\1 <REDACTED>"),
    (re.compile(r"cat/(?:temppwd|fish)"), "<REDACTED_USER>/<REDACTED>"),
    (re.compile(r"root/(?:123456|980923)"), "root/<REDACTED>"),
    (re.compile(r"root@192\.168\.3\.\d+[^\n]{0,30}(?:123456|980923)"),
     "root@<旧板地址> <REDACTED>"),
    (re.compile(r"`(?:temppwd|fish)`"), "<REDACTED>"),
]

PRIVATE_KEY = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")


def sanitize_text(text):
    for pat, rep in RULES:
        text = pat.sub(rep, text)
    if PRIVATE_KEY.search(text):
        text = PRIVATE_KEY.sub("-----BEGIN <PRIVATE KEY REDACTED>-----", text)
    # 规则替换后可能留下成对的 Markdown 反引号残片，统一收拾干净
    text = text.replace("<REDACTED>", "<REDACTED>").replace("<REDACTED>", "<REDACTED>")
    return text


def sanitize_file(path):
    if path.suffix.lower() not in TEXT_EXT and path.name not in ("Makefile", "Dockerfile"):
        return False
    try:
        old = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return False
    new = sanitize_text(old)
    if new == old:
        return False
    path.write_text(new, encoding="utf-8")
    return True


def find_leftovers(root):
    """导出后复查：只报明显口令串，不把普通数字误判为凭据。"""
    hits = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        if p.name == "sanitize.py":     # 规则本身含示例串，不是凭据
            continue
        if p.suffix.lower() not in TEXT_EXT and p.name not in ("Makefile", "Dockerfile"):
            continue
        try:
            t = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for pat in (re.compile(r"cat/(?:temppwd|fish)"),
                    re.compile(r"`(?:temppwd|fish)`"),
                    re.compile(r"(?<![\d.])980923(?![\d.])")):
            if pat.search(t):
                hits.append(str(p.relative_to(root)))
                break
    return sorted(set(hits))


if __name__ == "__main__":
    import sys
    target = Path(sys.argv[-1]) if len(sys.argv) > 1 else Path(".")
    found = find_leftovers(target)
    if found:
        print("发现疑似明文口令：")
        for f in found:
            print("  " + f)
        sys.exit(1)
    print("未发现明文设备口令")
