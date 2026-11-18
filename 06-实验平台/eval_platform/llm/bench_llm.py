#!/usr/bin/env python3
"""Benchmark the center LLM node (MacBook Air M1 / Ollama) on real edge-vision events.

Reads archived detection events from dashboard/samples/events_ai.jsonl, sends them
to the local Ollama OpenAI-compatible /api/generate endpoint and records measured
latency / decode throughput. No network access to other devices is required.
"""
import json
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EVENTS = ROOT / "eval_platform/dashboard/samples/events_ai.jsonl"
OUT = ROOT / "eval_platform/llm/llm_bench.json"
API = "http://127.0.0.1:11434/api/generate"
MODEL = "hetero-edge-llm:v1"


def generate(event, num_predict=110):
    payload = {
        "model": MODEL,
        "stream": False,
        "think": False,
        "keep_alive": "5m",
        "options": {"num_predict": num_predict, "temperature": 0.2},
        "prompt": ("请按固定三段格式处理下面这条端侧视觉检测事件：\n"
                   + json.dumps(event, ensure_ascii=False)),
    }
    req = urllib.request.Request(
        API,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    wall0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=180) as r:
        body = json.loads(r.read().decode("utf-8"))
    return body, time.monotonic() - wall0


def main():
    rows = [json.loads(l) for l in EVENTS.read_text().splitlines() if l.strip()]
    # representative events: high-confidence person, low-confidence car, tiny-area noise
    picks = []
    for pred in [
        lambda r: r["semantic"]["label"] == "person" and r["semantic"]["score"] >= 0.75 and r["area_px"] > 1000,
        lambda r: r["semantic"]["label"] == "car" and r["semantic"]["score"] < 0.4,
        lambda r: r["area_px"] < 15 and r["semantic"]["label"] == "none",
    ]:
        picks.append(next(r for r in rows if pred(r)))

    results = []
    for i, ev in enumerate(picks, 1):
        body, wall = generate(ev)
        ns = 1e9
        result = {
            "case": i,
            "event_ts_ms": ev["ts_ms"],
            "label": ev["semantic"]["label"],
            "score": ev["semantic"]["score"],
            "area_px": ev["area_px"],
            "response": body.get("response", "").strip(),
            "wall_s": round(wall, 3),
            "load_duration_s": round(body.get("load_duration", 0) / ns, 3),
            "prompt_eval_count": body.get("prompt_eval_count"),
            "prompt_eval_duration_s": round(body.get("prompt_eval_duration", 0) / ns, 3),
            "eval_count": body.get("eval_count"),
            "eval_duration_s": round(body.get("eval_duration", 0) / ns, 3),
            "decode_tok_per_s": round(
                body.get("eval_count", 0) / (body.get("eval_duration", 1) / ns), 2
            ),
            "total_duration_s": round(body.get("total_duration", 0) / ns, 3),
        }
        results.append(result)
        print(f"[case {i}] {result['decode_tok_per_s']} tok/s  "
              f"eval={result['eval_count']} tok  wall={result['wall_s']}s")
        print("   " + result["response"].replace("\n", " ")[:220])

    dec = [r["decode_tok_per_s"] for r in results]
    out = {
        "schema_version": "llm-deploy-bench-v1",
        "run_id": "20261113T145438Z_G4_m1_ascend_heterogeneous",
        "measured_local": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "node": "macbook-air-m1",
        "runtime": "Ollama 0.34.4 / Metal (llama.cpp backend)",
        "model": MODEL,
        "base_model": "qwen3:latest (8.2B, Q4_K_M, 5.23 GB)",
        "endpoint_local": API,
        "source_events": str(EVENTS.relative_to(ROOT)),
        "cases": results,
        "decode_tok_per_s_median": round(sorted(dec)[len(dec) // 2], 2),
        "notes": [
            "think=false，num_predict=110，temperature=0.2，num_ctx=8192，全层 GPU",
            "首次请求含 load_duration（模型冷加载），后续请求命中常驻内存",
            "仅本机回环实测；跨设备调用需先完成实验网静态地址配置",
        ],
    }
    OUT.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    print("\nwrote", OUT)


if __name__ == "__main__":
    main()
