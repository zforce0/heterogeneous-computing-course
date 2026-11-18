#!/usr/bin/env python3
"""Minimal TCP throughput / RTT probe for E04. iperf3 is only on RK3588,
so use this on all three nodes. Usage:
  tcp_bench.py sink  <port> <max_seconds>
  tcp_bench.py echo  <port> <max_seconds>
  tcp_bench.py client <host> <port> <seconds>
  tcp_bench.py rtt   <host> <port> <count>
"""
import json
import socket
import sys
import time

BLOCK = b"x" * 65536


def serve(kind, port, max_seconds):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(5)
    s.settimeout(max_seconds + 10)
    try:
        conn, addr = s.accept()
    except socket.timeout:
        print(json.dumps({"role": kind, "error": "no client"}))
        return
    conn.settimeout(1)
    total = 0
    start = time.monotonic()
    while time.monotonic() - start < max_seconds:
        try:
            data = conn.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        total += len(data)
        if kind == "echo":
            conn.sendall(data[:64])
    elapsed = time.monotonic() - start
    print(json.dumps({"role": kind, "peer": addr[0], "bytes": total,
                      "seconds": round(elapsed, 3),
                      "mbps": round(total * 8 / elapsed / 1e6, 2)}))
    conn.close()
    s.close()


def client(host, port, seconds):
    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(8)
    total = 0
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        s.sendall(BLOCK)
        total += len(BLOCK)
    elapsed = time.monotonic() - start
    s.close()
    print(json.dumps({"role": "client", "target": host, "bytes": total,
                      "seconds": round(elapsed, 3),
                      "mbps": round(total * 8 / elapsed / 1e6, 2)}))


def rtt(host, port, count):
    # echo server accepts one connection; reuse it for all samples
    samples = []
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(5)
    try:
        for _ in range(count):
            t0 = time.monotonic()
            s.sendall(b"p" * 64)
            s.recv(64)
            samples.append((time.monotonic() - t0) * 1000)
    finally:
        s.close()
    samples.sort()
    print(json.dumps({"role": "rtt", "target": host, "count": count,
                      "p50_ms": round(samples[len(samples) // 2], 3),
                      "p95_ms": round(samples[int(len(samples) * 0.95)], 3),
                      "min_ms": round(samples[0], 3),
                      "max_ms": round(samples[-1], 3)}))


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode in ("sink", "echo"):
        serve(mode, int(sys.argv[2]), int(sys.argv[3]))
    elif mode == "client":
        client(sys.argv[2], int(sys.argv[3]), float(sys.argv[4]))
    elif mode == "rtt":
        rtt(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
