#!/usr/bin/env python3
"""RV1126B 侧证据打包 + 发送（E09 证据协议 · 发送端）。

按事件打包（长度前缀 + run/node/event ID + 模型版本 + deadline），TCP 发 RK3588:9100，
收 ACK，超时按幂等 event_id 重试。使用 RV 自身单调钟（time.monotonic）测 RTT、建期限，
不依赖绝对墙钟——规避三端时钟漂移（RK/RV 的 RTC 均偏离 Mac）。

用法:
  python3 evidence_sender.py --server 192.168.77.21:9100 --duration 20 \
      --policy all --run-id e09_run_001
策略(--policy，供 E10 消融):
  all      : 每个有检测的新 seq 都发
  person   : 仅当存在 person 时发
  highconf : 仅保留 score>=0.5 的检测再发（过滤后为空则跳过）
  roi_top1 : 仅发置信度最高的单个 ROI
"""
import argparse
import json
import socket
import struct
import sys
import time

LIVE_BOXES = "/tmp/live_boxes.json"
STATUS_FILE = "/tmp/evidence_sender.json"
MODEL = "yolov8n"
MODEL_VERSION = "yolov8n-rknpu-1.0"
NODE = "rv1126b"
RETRIES = 3


def send_frame(sock, obj):
    payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf


def recv_frame(sock):
    hdr = recv_exact(sock, 4)
    (n,) = struct.unpack(">I", hdr)
    if n == 0 or n > 8 * 1024 * 1024:
        raise ValueError("bad frame length %d" % n)
    body = recv_exact(sock, n)
    return json.loads(body.decode("utf-8")), 4 + n


def ping_rtt(sock, run_id, samples=5):
    """用 RV 单调钟测同钟 RTT。"""
    rtts = []
    for i in range(samples):
        send_frame(sock, {"type": "ping", "run_id": run_id,
                          "event_id": "ping-%d" % i, "sent_mono_ms": time.monotonic()})
        t0 = time.monotonic()
        ack, _ = recv_frame(sock)
        rtts.append((time.monotonic() - t0) * 1000.0)
        if ack.get("type") != "pong":
            raise RuntimeError("unexpected ping reply: %r" % ack)
    rtts.sort()
    return rtts[len(rtts) // 2], rtts


def apply_policy(policy, dets):
    if policy == "person":
        dets = [d for d in dets if d.get("name") == "person"]
    elif policy == "highconf":
        dets = [d for d in dets if float(d.get("score", 0.0)) >= 0.5]
    elif policy == "roi_top1":
        dets = sorted(dets, key=lambda d: float(d.get("score", 0.0)), reverse=True)[:1]
    return dets


def connect(host, port, timeout=5.0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect((host, port))
    return sock


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="192.168.77.21:9100")
    ap.add_argument("--duration", type=float, default=20.0)
    ap.add_argument("--policy", default="all",
                    choices=["all", "person", "highconf", "roi_top1"])
    ap.add_argument("--run-id", default="e09_run_%d" % int(time.time()))
    ap.add_argument("--max-events", type=int, default=0)
    ap.add_argument("--model-version", default=MODEL_VERSION)
    ap.add_argument("--deadline-ms", type=float, default=0.0,
                    help="强制 ACK 等待期限(ms)；0=按 5xRTT 自适应（E11 用受限链路时设紧）")
    args = ap.parse_args()

    host, port = args.server.split(":")
    port = int(port)

    # 对账统计（发送端视角）
    acc = {
        "run_id": args.run_id, "node": NODE, "policy": args.policy,
        "model": MODEL, "model_version": args.model_version,
        "started_at": time.time(), "server": args.server,
        "sent": 0, "unique_sent": 0, "acked": 0,
        "confirmed": 0, "rejected": 0, "duplicate_acked": 0,
        "timeout": 0, "retries": 0, "dropped": 0,
        "bytes_out": 0, "bytes_in": 0,
        "rtt_ms": [], "infer_ms": [], "reconf_ms": [],
        "deadline_ms": 0, "last_seq": 0, "last_error": "",
    }

    try:
        sock = connect(host, port)
    except OSError as exc:
        acc["last_error"] = "connect_failed: %s" % exc
        with open(STATUS_FILE, "w") as f:
            json.dump(acc, f, ensure_ascii=False, indent=2)
        print(json.dumps(acc, ensure_ascii=False))
        sys.exit(2)

    try:
        median_rtt, all_rtt = ping_rtt(sock, args.run_id)
        acc["rtt_ms"] = [round(r, 2) for r in all_rtt]
        acc["deadline_ms"] = (args.deadline_ms if args.deadline_ms > 0
                              else max(200.0, round(5.0 * median_rtt)))
    except (OSError, ConnectionError, RuntimeError) as exc:
        acc["last_error"] = "ping_failed: %s" % exc
        with open(STATUS_FILE, "w") as f:
            json.dump(acc, f, ensure_ascii=False, indent=2)
        print(json.dumps(acc, ensure_ascii=False))
        sys.exit(3)

    deadline_s = acc["deadline_ms"] / 1000.0
    deadline = time.monotonic() + args.duration
    sent_ids = set()
    last_seq = 0
    ev_counter = 0

    while time.monotonic() < deadline:
        if args.max_events and acc["unique_sent"] >= args.max_events:
            break
        # 读取最新检测帧（按 seq 去重，仅发新 seq）
        try:
            live = json.loads(open(LIVE_BOXES).read())
        except Exception:
            time.sleep(0.05)
            continue
        seq = int(live.get("seq", 0))
        if seq == last_seq:
            time.sleep(0.03)
            continue
        last_seq = seq
        dets = apply_policy(args.policy, live.get("detections", []))
        if not dets:
            continue
        if live.get("infer_ms") is not None:
            acc["infer_ms"].append(float(live["infer_ms"]))

        ev_counter += 1
        event_id = "%s:%d" % (args.run_id, seq)
        obj = {
            "type": "evt",
            "run_id": args.run_id, "node": NODE, "event_id": event_id,
            "model": MODEL, "model_version": args.model_version,
            "seq": seq, "infer_ms": live.get("infer_ms"),
            "ts_ms": live.get("ts_ms"),
            "deadline_ms": acc["deadline_ms"],
            "detections": dets,
        }
        payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")

        # 幂等重试：同一 event_id 最多重发 RETRIES 次
        acked = False
        for attempt in range(1, RETRIES + 1):
            try:
                sock.sendall(struct.pack(">I", len(payload)) + payload)
                acc["sent"] += 1
                acc["bytes_out"] += len(payload)
                sock.settimeout(deadline_s)
                t0 = time.monotonic()
                ack, nbytes = recv_frame(sock)
                rtt = (time.monotonic() - t0) * 1000.0
                acc["bytes_in"] += nbytes
                acc["acked"] += 1
                acc["rtt_ms"].append(round(rtt, 2))
                if ack.get("reconf_ms") is not None:
                    acc["reconf_ms"].append(float(ack["reconf_ms"]))
                if ack.get("verdict") == "confirm":
                    acc["confirmed"] += 1
                elif ack.get("verdict") == "duplicate":
                    acc["duplicate_acked"] += 1
                else:
                    acc["rejected"] += 1
                acked = True
                break
            except (socket.timeout, ConnectionError, OSError, ValueError) as exc:
                acc["retries"] += 1
                if attempt == RETRIES:
                    acc["timeout"] += 1
                    acc["dropped"] += 1
                    acc["last_error"] = "timeout seq=%d: %s" % (seq, exc)
                # 重连后重试（连接可能已断）
                try:
                    sock.close()
                except OSError:
                    pass
                try:
                    sock = connect(host, port)
                except OSError:
                    pass
        if acked:
            sent_ids.add(event_id)
        acc["unique_sent"] = len(sent_ids)
        acc["last_seq"] = seq
        # 周期性落盘状态，供 dashboard / 对账读取
        with open(STATUS_FILE, "w") as f:
            json.dump(acc, f, ensure_ascii=False, indent=2)

    # 汇总 RTT 分位数
    rtts = sorted(acc["rtt_ms"])
    if rtts:
        acc["rtt_min"] = round(rtts[0], 2)
        acc["rtt_median"] = round(rtts[len(rtts) // 2], 2)
        acc["rtt_p95"] = round(rtts[int(len(rtts) * 0.95)], 2)
        acc["rtt_max"] = round(rtts[-1], 2)
    infers = sorted(acc["infer_ms"])
    acc["infer_ms_median"] = round(infers[len(infers) // 2], 2) if infers else None
    reconfs = sorted(acc["reconf_ms"])
    acc["reconf_ms_median"] = round(reconfs[len(reconfs) // 2], 4) if reconfs else None
    # 综合推理时延（端到端）= 中位 RV 推理 + 中位传输 RTT + 中位 RK 复核
    acc["e2e_ms"] = round((acc.get("infer_ms_median") or 0)
                          + (acc.get("rtt_median") or 0)
                          + (acc.get("reconf_ms_median") or 0), 2)
    # 去掉原始样本列表，只留分位数
    for k in ("rtt_ms", "infer_ms", "reconf_ms"):
        acc.pop(k, None)
    acc["duration_s"] = round(time.time() - acc["started_at"], 2)
    with open(STATUS_FILE, "w") as f:
        json.dump(acc, f, ensure_ascii=False, indent=2)
    print(json.dumps(acc, ensure_ascii=False, indent=2))
    try:
        sock.close()
    except OSError:
        pass


if __name__ == "__main__":
    main()
