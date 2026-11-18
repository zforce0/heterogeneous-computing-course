#!/usr/bin/env python3
"""G4 · RK3588 侧「端侧视觉事件 → 昇腾 LLM 语义二次确认」客户端。

架构：Mac 仅显示/控制；昇腾盒子 (192.168.77.10:8080) 是唯一 LLM 节点；
RK3588 保留 9100 证据服务（规则复核），并把高价值事件送 LLM 做语义复核。

在 RK3588 上运行（Python 3.9 标准库即可，无第三方依赖）：
    python3 rk_llm_reconfirm.py --events /home/cat/eval_dashboard/events_ai.jsonl \
        --out /home/cat/eval_dashboard/llm_reconfirm.json

也可用 --api 覆盖端点。失败时如实写 error，不伪造结论。
"""
import argparse
import json
import re
import time
import urllib.request

DEFAULT_API = "http://192.168.77.10:8080/v1/chat/completions"
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


def call_llm(api, event, timeout=120):
    payload = {
        "model": "g4-edge-llm",
        "messages": [{"role": "user", "content": FEWSHOT + summarize(event)}],
        "temperature": 0,
        "max_tokens": 12,
        "stream": False,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    req = urllib.request.Request(
        api,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = json.loads(r.read().decode("utf-8"))
    dt = time.monotonic() - t0
    text = body["choices"][0]["message"]["content"].strip()
    usage = body.get("usage", {})
    return text, dt, usage


def parse_verdict(text):
    """极简提示词下模型只输出一个词；同时兼容 JSON / 弱格式输出。"""
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default=DEFAULT_API)
    ap.add_argument("--events", default="/home/cat/eval_dashboard/events_ai.jsonl")
    ap.add_argument("--out", default="/home/cat/eval_dashboard/llm_reconfirm.json")
    ap.add_argument("--limit", type=int, default=3)
    args = ap.parse_args()

    rows = [json.loads(l) for l in open(args.events, encoding="utf-8") if l.strip()]
    # 代表性事件：高置信大面积 person / 低置信 car / 极小面积噪声
    picks = []
    rules = [
        lambda r: r["semantic"]["label"] == "person" and r["semantic"]["score"] >= 0.75 and r["area_px"] > 1000,
        lambda r: r["semantic"]["label"] == "car" and r["area_px"] < 300,
        lambda r: r["area_px"] < 15 and r["semantic"]["label"] == "none",
    ]
    for pred in rules:
        try:
            picks.append(next(r for r in rows if pred(r)))
        except StopIteration:
            pass
    picks = picks[:args.limit]

    results, ok = [], 0
    for i, ev in enumerate(picks, 1):
        rec = {"seq": i, "event_id": ev.get("id"), "ts_ms": ev.get("ts_ms"),
               "label": ev["semantic"]["label"], "score": ev["semantic"]["score"],
               "area_px": ev.get("area_px"), "llm_node": "ascend-box"}
        try:
            text, dt, usage = call_llm(args.api, ev)
            rec.update({"llm_text": text, "verdict": parse_verdict(text),
                        "wall_s": round(dt, 3), "usage": usage, "status": "ok"})
            ok += 1
        except Exception as exc:
            rec.update({"status": "error", "error": str(exc)[:200]})
        results.append(rec)
        print("[%d/%d] event=%s label=%s -> %s (%.2fs)" %
              (i, len(picks), rec["event_id"], rec["label"], rec.get("status"), rec.get("wall_s", 0)))

    out = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "node": "rk3588", "role": "edge-vision -> ascend-llm-reconfirm",
        "api": args.api, "ok": ok, "total": len(picks), "results": results,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print("written:", args.out, "ok=%d/%d" % (ok, len(picks)))
    return 0 if ok == len(picks) and ok > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
