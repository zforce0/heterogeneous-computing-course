#!/usr/bin/env python3
"""RK3588 侧证据接收 + 规则二次确认（E09 证据协议 · 接收端）。

协议（长度前缀 TCP，端口 9100，绑定 192.168.77.21）：
  帧 = [4 字节大端长度][UTF-8 JSON 载荷]
  载荷（RV -> RK）:
    {"type":"evt","run_id":..,"node":"rv1126b","event_id":..,
     "model":"yolov8n","model_version":..,"seq":..,"infer_ms":..,
     "ts_ms":..,"deadline_ms":..,"detections":[{"name","score","box":[x1,y1,x2,y2]}]}
  ACK（RK -> RV）:
    {"type":"ack","run_id":..,"event_id":..,"verdict":"confirm|reject|duplicate",
     "per_detection":[{"name","score","area_px","aspect","verdict","reason"}],
     "received_at_ms":..,"node":"rk3588","reconf":"rule"}

二次确认（异构协同推理的第二棒）：RK3588 用确定性规则对每个 ROI 复核几何与置信度
（RKNN 全模型 ROI 再推理为可升级路径，见 README 说明）。规则集见 rule_verdict()。
幂等：以 (run_id, event_id) 去重，重复帧返回缓存 ACK 并计入 duplicate。
"""
import json
import random
import socket
import struct
import threading
import time
import sys

HOST = "0.0.0.0"          # 实际绑定 192.168.77.21 可达
PORT = 9100
STATUS_FILE = "/tmp/evidence_server.json"

# 规则阈值（确定性、可复核）
MIN_AREA_PX = 400         # 最小 ROI 面积（像素），过小视为噪声
MIN_ASPECT = 0.1          # 宽高比下限
MAX_ASPECT = 10.0         # 宽高比上限
PERSON_MIN_SCORE = 0.45   # person 类二次确认分数门槛
GENERIC_MIN_SCORE = 0.30  # 其他类通用分数门槛


def rule_verdict(det):
    """对单个检测 ROI 做规则复核，返回 (verdict, reason, area_px, aspect)。"""
    name = det.get("name", "?")
    score = float(det.get("score", 0.0))
    box = det.get("box", [0, 0, 0, 0])
    x1, y1, x2, y2 = (float(v) for v in box[:4])
    w = x2 - x1
    h = y2 - y1
    area = max(0.0, w) * max(0.0, h)
    aspect = (w / h) if h > 1e-6 else 99.0

    if w <= 0 or h <= 0:
        return "reject", "invalid_bbox", area, aspect
    if area < MIN_AREA_PX:
        return "reject", "area_too_small", area, aspect
    if aspect < MIN_ASPECT or aspect > MAX_ASPECT:
        return "reject", "aspect_out_of_range", area, aspect
    if name == "person" and score < PERSON_MIN_SCORE:
        return "reject", "person_score_low", area, aspect
    if score < GENERIC_MIN_SCORE:
        return "reject", "score_low", area, aspect
    return "confirm", "geometry+score_ok", area, aspect


class EvidenceServer:
    def __init__(self, host, port, delay_ms=0.0, drop_rate=0.0, status_file=STATUS_FILE):
        self.host, self.port = host, port
        self.delay_ms = delay_ms      # 链路仿真：附加单向时延(ms)
        self.drop_rate = drop_rate    # 链路仿真：回程 ACK 丢包率(0~1)
        self.status_file = status_file
        self.lock = threading.Lock()
        self.dedup = {}        # (run_id, event_id) -> cached ack (per_detection list)
        self.stats = {
            "started_at": time.time(),
            "received": 0, "unique": 0, "duplicate": 0,
            "confirmed": 0, "rejected": 0, "ack_dropped": 0,
            "bytes_in": 0, "by_class": {}, "runs": {},
            "reconf_ms_samples": [],   # RK 规则复核耗时样本（用于综合推理时延）
            "last_event_ms": 0, "delay_ms": delay_ms, "drop_rate": drop_rate,
        }
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(16)

    def recv_exact(self, conn, n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("peer closed")
            buf += chunk
        return buf

    def recv_frame(self, conn):
        hdr = self.recv_exact(conn, 4)
        (n,) = struct.unpack(">I", hdr)
        if n == 0 or n > 8 * 1024 * 1024:
            raise ValueError("bad frame length %d" % n)
        return self.recv_exact(conn, n), 4 + n

    def send_frame(self, conn, obj):
        payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        conn.sendall(struct.pack(">I", len(payload)) + payload)

    def handle(self, conn, addr):
        try:
            while True:
                raw, nbytes = self.recv_frame(conn)
                obj = json.loads(raw.decode("utf-8"))
                is_ping = obj.get("type") == "ping"
                # 链路仿真：附加单向时延（对 ping/evt 均生效，用于测真实 RTT）
                if self.delay_ms > 0:
                    time.sleep(self.delay_ms / 1000.0)
                with self.lock:
                    self.stats["bytes_in"] += nbytes
                t0 = time.monotonic()
                ack = self.process(obj)
                reconf_ms = (time.monotonic() - t0) * 1000.0
                if not is_ping:
                    with self.lock:
                        self.stats["reconf_ms_samples"].append(reconf_ms)
                if isinstance(ack, dict) and ack.get("type") == "ack":
                    ack["reconf_ms"] = round(reconf_ms, 4)
                # 链路仿真：回程 ACK 丢包（仅 evt，ping/pong 不丢，保证测 RTT 稳定）
                if (not is_ping) and self.drop_rate > 0 and random.random() < self.drop_rate:
                    with self.lock:
                        self.stats["ack_dropped"] += 1
                    continue  # 不发送 ACK，发送端超时后按幂等 event_id 重试
                self.send_frame(conn, ack)
        except (ConnectionError, OSError, ValueError, json.JSONDecodeError) as exc:
            # 帧损坏/断连：静默丢弃，不污染对账（发送端负责超时重试）
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def process(self, obj):
        now_ms = int(time.time() * 1000)
        if obj.get("type") == "ping":
            return {"type": "pong", "run_id": obj.get("run_id", "?"),
                    "event_id": obj.get("event_id", "?"), "at_ms": now_ms,
                    "node": "rk3588"}
        run_id = obj.get("run_id", "?")
        event_id = obj.get("event_id", "?")
        dets = obj.get("detections", [])
        key = (run_id, event_id)

        with self.lock:
            self.stats["received"] += 1
            self.stats["last_event_ms"] = now_ms
            # 去重：已见过则返回缓存 ACK，标记 duplicate
            if key in self.dedup:
                self.stats["duplicate"] += 1
                run = self.stats["runs"].setdefault(run_id, {
                    "received": 0, "unique": 0, "duplicate": 0,
                    "confirmed": 0, "rejected": 0, "first_ms": now_ms, "last_ms": now_ms,
                })
                run["received"] += 1
                run["duplicate"] += 1
                run["last_ms"] = now_ms
                cached = dict(self.dedup[key])
                cached["verdict"] = "duplicate"
                cached["received_at_ms"] = now_ms
                cached["duplicate"] = True
                return cached

            per = []
            n_conf = 0
            for det in dets:
                verdict, reason, area, aspect = rule_verdict(det)
                if verdict == "confirm":
                    n_conf += 1
                per.append({
                    "name": det.get("name", "?"),
                    "score": round(float(det.get("score", 0.0)), 4),
                    "area_px": round(area, 1),
                    "aspect": round(aspect, 3),
                    "verdict": verdict,
                    "reason": reason,
                })
                cls = det.get("name", "?")
                self.stats["by_class"][cls] = self.stats["by_class"].get(cls, 0) + 1

            event_verdict = "confirm" if n_conf > 0 else "reject"
            self.stats["unique"] += 1
            if event_verdict == "confirm":
                self.stats["confirmed"] += 1
            else:
                self.stats["rejected"] += 1

            run = self.stats["runs"].setdefault(run_id, {
                "received": 0, "unique": 0, "duplicate": 0,
                "confirmed": 0, "rejected": 0, "first_ms": now_ms, "last_ms": now_ms,
            })
            run["received"] += 1
            run["unique"] += 1
            run["confirmed" if event_verdict == "confirm" else "rejected"] += 1
            run["last_ms"] = now_ms

            ack = {
                "type": "ack",
                "run_id": run_id,
                "event_id": event_id,
                "verdict": event_verdict,
                "per_detection": per,
                "received_at_ms": now_ms,
                "node": "rk3588",
                "reconf": "rule",
            }
            self.dedup[key] = {
                "type": "ack", "run_id": run_id, "event_id": event_id,
                "verdict": event_verdict, "per_detection": per,
                "node": "rk3588", "reconf": "rule",
            }
            self.dedup[key]["received_at_ms"] = now_ms
            # 限制去重表大小，防止内存无限增长
            if len(self.dedup) > 200000:
                for k in list(self.dedup)[:100000]:
                    self.dedup.pop(k, None)
        return ack

    def write_status(self):
        while True:
            time.sleep(1.0)
            with self.lock:
                snap = json.loads(json.dumps(self.stats))
                snap["uptime_s"] = round(time.time() - self.stats["started_at"], 1)
                snap["dedup_size"] = len(self.dedup)
                samples = self.stats.get("reconf_ms_samples", [])
                if samples:
                    s = sorted(samples)
                    snap["reconf_ms"] = {
                        "count": len(s),
                        "min": round(s[0], 4),
                        "median": round(s[len(s) // 2], 4),
                        "p95": round(s[int(len(s) * 0.95)], 4),
                        "max": round(s[-1], 4),
                    }
                snap.pop("reconf_ms_samples", None)
            try:
                with open(self.status_file, "w") as f:
                    json.dump(snap, f, ensure_ascii=False, indent=2)
            except OSError:
                pass

    def run(self):
        threading.Thread(target=self.write_status, daemon=True).start()
        print("evidence_server listening on %s:%d" % (self.host, self.port), flush=True)
        while True:
            conn, addr = self.sock.accept()
            threading.Thread(target=self.handle, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default=HOST)
    ap.add_argument("port", nargs="?", type=int, default=PORT)
    ap.add_argument("--delay-ms", type=float, default=0.0,
                    help="链路仿真：附加单向时延(ms)")
    ap.add_argument("--drop-rate", type=float, default=0.0,
                    help="链路仿真：回程 ACK 丢包率(0~1)")
    ap.add_argument("--status-file", default=STATUS_FILE)
    a = ap.parse_args()
    EvidenceServer(a.host, a.port, a.delay_ms, a.drop_rate, a.status_file).run()
