"""实验室参数解析：config/lab.env + 进程环境变量，缺省回退课程示例值。

设计目的：把原来散落在 serve_g4.py / build_g4_dashboard.py / 若干 shell 脚本里的
实验网地址、端口、SSH 别名收敛到一处。新机器只需复制 config/lab.env.example
为 config/lab.env 并改地址，无需改代码；不配置时行为与改动前完全一致。

纯标准库；不读取任何凭据文件，也不保存口令。
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENV_FILE = ROOT / "config" / "lab.env"

# 与 config/lab.env.example 保持一致，未配置时即原实验网设置
DEFAULTS = {
    "MAC_IP": "192.168.77.1",
    "ASCEND_IP": "192.168.77.10",
    "RK_IP": "192.168.77.21",
    "RV_IP": "192.168.77.22",
    "DASH_HOST": "127.0.0.1",
    "DASH_PORT": "8899",
    "LLM_PORT": "8080",
    "RV_VISION_PORT": "8082",
    "RK_DASH_PORT": "8081",
    "RK_EVIDENCE_PORT": "9100",
    "SSH_ASCEND": "ascend",
    "SSH_RK3588": "rk3588",
    "SSH_RV1126B": "rv1126b",
    "LLM_MODEL_ALIAS": "g4-edge-llm",
    "VIS_MODEL": "yolov8m_rv1126b.rknn",
    "IR_MODEL": "yolov8m_rv1126b.rknn",
}

_PORTS = {
    "llm": "LLM_PORT",
    "vision": "RV_VISION_PORT",
    "dash": "RK_DASH_PORT",
    "evidence": "RK_EVIDENCE_PORT",
}
_IPS = {
    "mac": "MAC_IP",
    "ascend": "ASCEND_IP",
    "rk3588": "RK_IP",
    "rv1126b": "RV_IP",
}
_SSH = {
    "mac": None,
    "ascend": "SSH_ASCEND",
    "rk3588": "SSH_RK3588",
    "rv1126b": "SSH_RV1126B",
}


def _read_env_file(path):
    out = {}
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if not s or s.startswith("#") or "=" not in s:
                continue
            k, v = s.split("=", 1)
            k, v = k.strip(), v.strip().strip('"').strip("'")
            if k in DEFAULTS:
                out[k] = v
    except OSError:
        pass
    return out


def load():
    """文件值优先于默认值，进程环境变量又优先于文件值。"""
    cfg = dict(DEFAULTS)
    cfg.update(_read_env_file(ENV_FILE))
    cfg.update({k: v for k, v in os.environ.items() if k in DEFAULTS})
    return cfg


CFG = load()


def get(key, default=None):
    return CFG.get(key, default)


def ip(role):
    return CFG[_IPS[role]]


def port(kind):
    return CFG[_PORTS[kind]]


def endpoint(role, kind):
    return "http://%s:%s" % (ip(role), port(kind))


def ssh(role):
    k = _SSH[role]
    return CFG[k] if k else None


def dash_bind():
    return CFG["DASH_HOST"], int(CFG["DASH_PORT"])
