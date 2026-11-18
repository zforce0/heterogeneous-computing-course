#!/usr/bin/env python3
"""Serve the benchmarking dashboard plus a same-origin camera proxy.

Mac:      python3 serve.py --host 192.168.77.1 --port 8080
Boards:   python3 serve.py --host <board-ip> --port 8081
          (eval-dashboard.service)

/vis.jpg /ir.png /status.json /control are proxied to the RV1126B camera
preview at 192.168.77.22:8082, so browsers only talk to their own origin.
No third-party dependency; never binds 0.0.0.0.
"""
import argparse
import functools
import http.server
import pathlib
import socketserver
import urllib.request

CAM_ORIGIN = "http://192.168.77.22:8082"
PROXY_PATHS = ("/vis.jpg", "/ir.png", "/status.json", "/control")
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in PROXY_PATHS:
            try:
                with OPENER.open(CAM_ORIGIN + self.path, timeout=6) as r:
                    body = r.read()
                    ctype = r.headers.get("Content-Type", "application/octet-stream")
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Private-Network", "true")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as exc:
                msg = ("camera upstream unavailable: %s" % exc).encode()
                self.send_response(502)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(msg)))
                self.end_headers()
                self.wfile.write(msg)
            return
        super().do_GET()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    root = pathlib.Path(__file__).resolve().parent
    handler = functools.partial(Handler, directory=str(root))
    with Server((a.host, a.port), handler) as srv:
        print("dashboard: http://%s:%d/ (camera proxy -> %s)" % (a.host, a.port, CAM_ORIGIN), flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
