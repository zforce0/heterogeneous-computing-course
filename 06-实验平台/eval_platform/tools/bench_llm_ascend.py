#!/usr/bin/env python3
"""G4 · 从 Mac（显示/控制端）实测昇腾盒子上的中心 LLM 服务。

注意：Mac 不再承担推理，只发请求与记录结果。
用法: python3 eval_platform/tools/bench_llm_ascend.py [--api http://192.168.77.10:8080]
"""
import argparse
import json
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EVENTS = ROOT / "eval_platform/dashboard/samples/events_ai.jsonl"
OUT = ROOT / "eval_platform/llm/llm_bench_ascend.json"

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


def call(api, event, max_tokens=12):
    payload = {
        "model": "g4-edge-llm",
        "messages": [{"role": "user", "content": FEWSHOT + summarize(event)}],
        "temperature": 0,
        "max_tokens": max_tokens,
        "stream": False,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    req = urllib.request.Request(
        api.rstrip("/") + "/v1/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=180) as r:
        body = json.loads(r.read().decode("utf-8"))
    wall = time.monotonic() - t0
    return body, wall


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default="http://192.168.77.10:8080")
    args = ap.parse_args()

    rows = [json.loads(l) for l in EVENTS.read_text(encoding="utf-8").splitlines() if l.strip()]
    rules = [
        ("high_conf_person", lambda r: r["semantic"]["label"] == "person" and r["semantic"]["score"] >= 0.75 and r["area_px"] > 1000),
        ("low_conf_car", lambda r: r["semantic"]["label"] == "car" and r["area_px"] < 300),
        ("tiny_noise", lambda r: r["area_px"] < 15 and r["semantic"]["label"] == "none"),
    ]
    results = []
    for name, pred in rules:
        ev = next((r for r in rows if pred(r)), None)
        if ev is None:
            print("[SKIP] no event for", name)
            continue
        try:
            body, wall = call(args.api, ev)
            ch = body["choices"][0]
            usage = body.get("usage", {})
            gen = usage.get("completion_tokens", 0)
            timings = body.get("timings", {})
            tps = timings.get("predicted_per_second")
            text = ch["message"]["content"].strip()
            rec = {
                "case": name, "event_id": ev.get("id"), "ts_ms": ev.get("ts_ms"),
                "label": ev["semantic"]["label"], "score": ev["semantic"]["score"],
                "area_px": ev.get("area_px"),
                "status": "ok", "wall_s": round(wall, 3),
                "prompt_tokens": usage.get("prompt_tokens"),
                "completion_tokens": gen,
                "decode_tok_per_s": round(tps, 2) if tps else (round(gen / wall, 2) if wall else None),
                "prompt_tok_per_s": round(timings["prompt_per_second"], 2) if timings.get("prompt_per_second") else None,
                "verdict": "confirm" if "confirm" in text.lower() else ("reject" if "reject" in text.lower() else "unknown"),
                "finish_reason": ch.get("finish_reason"),
                "llm_text": text[:200],
            }
        except Exception as exc:
            rec = {"case": name, "status": "error", "error": str(exc)[:300]}
        results.append(rec)
        print("[%s] status=%s wall=%s tok/s=%s" %
              (name, rec["status"], rec.get("wall_s"), rec.get("decode_tok_per_s")))

    ok = [r for r in results if r["status"] == "ok"]
    out = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "architecture": "Mac = display/control ONLY; Ascend box = central LLM",
        "llm_node": "ascend-box (QiZhi01-20T / Atlas 200I A2 / 310B1)",
        "api": args.api,
        "model": "qwen3:0.6b Q4 (GGUF, transferred from Mac Ollama blob)",
        "ok": len(ok), "total": len(results),
        "median_decode_tok_per_s": (sorted(r["decode_tok_per_s"] for r in ok)[len(ok) // 2] if ok else None),
        "results": results,
    }
    OUT.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("written:", OUT)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
