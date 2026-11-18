#!/usr/bin/env python3
"""G4 · 用昇腾盒子上的中心 LLM 重新给 4 组带时间戳的关键帧做语义二次确认。

Mac 只负责编排/显示；所有推理都在 192.168.77.10:8080。
输出: eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous/keyframes_ascend.json
"""
import json
import re
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "eval_platform/runs/20261113T145438Z_G4_m1_ascend_heterogeneous"
SRC = RUN / "keyframes.json"
OUT = RUN / "keyframes_ascend.json"
EVENTS = ROOT / "eval_platform/dashboard/samples/events_ai.jsonl"
API = "http://192.168.77.10:8080/v1/chat/completions"

# 0.6B 小模型对长 JSON 提示词遵循度差，改用紧凑特征摘要 + 少样本，只输出一个词。
FEWSHOT = ("端侧检测事件判断。只回答一个词 confirm 或 reject。\n"
           "示例：标签person 置信度0.76 面积1393px 温差8.6K 持续15.1s -> confirm\n"
           "示例：标签none 置信度0.0 面积11px 温差0.2K 持续0.0s -> reject\n"
           "事件：")


def summarize(event):
    sem = event.get("semantic", {})
    tmp = event.get("temp", {})
    return ("标签%s 置信度%s 面积%spx 温差%sK 持续%ss ->" %
            (sem.get("label", "none"), sem.get("score", 0),
             event.get("area_px", 0), tmp.get("delta_t", 0),
             event.get("temporal", {}).get("persistence_s",
                                          event.get("persistence_s", 0))))


def call(event):
    payload = {
        "model": "g4-edge-llm",
        "messages": [{"role": "user", "content": FEWSHOT + summarize(event)}],
        "temperature": 0, "max_tokens": 12, "stream": False,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    req = urllib.request.Request(
        API, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=180) as r:
        body = json.loads(r.read().decode("utf-8"))
    dt = time.monotonic() - t0
    text = body["choices"][0]["message"]["content"].strip()
    return text, dt, body.get("usage", {})


def parse(text):
    """极简提示词下模型只输出一个词；同时兼容 JSON 与弱格式输出。"""
    t = text.strip().lower()
    verdict = "confirm" if "confirm" in t else ("reject" if "reject" in t else "unknown")
    reason = ""
    m = re.search(r'reason\s*[:=(]\s*"?([^",)}\n]{1,30})', t)
    if m:
        reason = m.group(1).strip()
    need = None
    if "need_review" in t:
        need = not re.search(r'need_review\s*[:=]\s*false', t) and not re.search(r'need_review\(false\)', t)
    return {"verdict": verdict, "reason": reason or ("规则一致" if verdict != "unknown" else "未识别输出"),
            "need_review": need}


def main():
    d = json.loads(SRC.read_text(encoding="utf-8"))
    events = {r["id"]: r for r in
              (json.loads(l) for l in EVENTS.read_text(encoding="utf-8").splitlines() if l.strip())}
    frames = []
    for i, f in enumerate(d["keyframes"], 1):
        ev = events.get(f["event_id"])
        payload = ev or {k: f[k] for k in ("semantic", "area_px", "temp", "persistence_s")}
        rec = dict(f)
        rec["kf_id"] = "KF-%02d" % i
        rec["llm_node"] = "ascend-box"
        try:
            text, dt, usage = call(payload)
            rec["llm_verdict"] = parse(text)
            rec["llm_raw"] = text[:300]
            rec["llm_wall_s"] = round(dt, 3)
            rec["llm_usage"] = usage
            rec["llm_status"] = "ok"
        except Exception as exc:
            rec["llm_verdict"] = {"verdict": "N/A", "reason": "服务不可用"}
            rec["llm_status"] = "error"
            rec["llm_error"] = str(exc)[:200]
        frames.append(rec)
        print("[%s] %s event=%s label=%s -> %s (%.2fs)" %
              (rec["kf_id"], rec["frame_time_local"], f["event_id"],
               f["semantic"]["label"], rec["llm_verdict"].get("verdict"), rec.get("llm_wall_s", 0)))

    d["keyframes"] = frames
    d["center_llm"] = {
        "node": "昇腾 QiZhi01-20T / Atlas 200I A2 / 310B1",
        "model": "qwen3:0.6b Q4 GGUF", "runtime": "llama.cpp (compiled on board)",
        "api": API, "mac_role": "display/control only (no inference)",
    }
    d["generated_local"] = time.strftime("%Y-%m-%d %H:%M:%S %z")
    d["schema_version"] = "g4-ascend-llm-v1"
    d["honest_note"] = ("关键帧图像沿用 2026-10-12 RV1126B 板端 dual_demo 归档实跑，"
                        "标注 archived；本轮仅把语义二次确认由 Mac Ollama 迁移到昇腾盒子 LLM。")
    OUT.write_text(json.dumps(d, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("written:", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
