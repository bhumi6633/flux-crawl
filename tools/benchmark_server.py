"""Deterministic, concurrent HTTP/1.0 benchmark site; binds IPv4 loopback only."""
import argparse
from collections import Counter
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler
import threading
import time
from local_http import LocalHttpServer


@dataclass(frozen=True)
class BenchmarkScenario:
    pages: int
    fanout: int
    delay_ms: int
    body_bytes: int


SCENARIOS = {
    "fast-small": BenchmarkScenario(100, 4, 2, 1024),
    "latency-bound": BenchmarkScenario(100, 4, 50, 1024),
    "higher-latency": BenchmarkScenario(100, 4, 150, 1024),
    "high-fanout": BenchmarkScenario(100, 99, 5, 4096),
    "large-body": BenchmarkScenario(100, 4, 10, 65536),
}


def scenario_workload(name, **overrides):
    if name not in SCENARIOS:
        raise ValueError("unknown benchmark scenario")
    preset = SCENARIOS[name]
    parameters = {field: getattr(preset, field) for field in ("pages", "fanout", "delay_ms", "body_bytes")}
    parameters.update({key: value for key, value in overrides.items() if value is not None})
    return Workload(**parameters, scenario=name)


class Workload:
    def __init__(self, pages=50, fanout=4, delay_ms=20, body_bytes=1024, scenario="custom"):
        if scenario != "custom" and scenario not in SCENARIOS:
            raise ValueError("unknown benchmark scenario")
        self.scenario = scenario
        if not 1 <= pages <= 100000 or not 1 <= fanout <= 1000:
            raise ValueError("pages must be 1..100000 and fanout 1..1000")
        if not 0 <= delay_ms <= 10000 or not 1 <= body_bytes <= 1048576:
            raise ValueError("delay-ms must be 0..10000 and body-bytes 1..1048576")
        self.pages, self.fanout = pages, fanout
        self.delay_ms, self.body_bytes = delay_ms, body_bytes
        # Validate exact sizes at startup; generate on demand to avoid pages * body memory.
        for page in range(pages):
            if len(self.markup(page)) > body_bytes:
                raise ValueError("body-bytes too small for generated links")

    def children(self, page):
        return range(page * self.fanout + 1, min(page * self.fanout + self.fanout + 1, self.pages))

    def markup(self, page):
        links = "".join(f'<a href="/page/{child}">L</a>' for child in self.children(page))
        return f"<!doctype html><html><body>{links}</body></html>".encode("ascii")

    def body(self, page):
        markup = self.markup(page)
        return markup + b" " * (self.body_bytes - len(markup))


class BenchmarkServer(LocalHttpServer):
    def __init__(self, port, workload):
        self.workload = workload
        self.lock = threading.Lock()
        self.counts = Counter()
        self.active = self.peak = self.resets = 0
        super().__init__(("127.0.0.1", port), Handler)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_body(self, status, body, content_type="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        server, workload = self.server, self.server.workload
        if self.path == "/__benchmark/config":
            text = f"fluxcrawl-benchmark-v2 {workload.pages} {workload.fanout} {workload.delay_ms} {workload.body_bytes} {workload.scenario}\n"
            self.send_body(200, text.encode())
            return
        if self.path == "/__benchmark/reset":
            with server.lock:
                idle = server.active == 0
                if idle:
                    server.resets += 1
                    server.counts.clear()
                    server.peak = 0
            self.send_body(200 if idle else 409, b"ok\n" if idle else b"requests still active\n")
            return
        if self.path == "/__benchmark/stats":
            with server.lock:
                text = f"{sum(server.counts.values())} {server.active} {server.peak} {len(server.counts)}\n"
            self.send_body(200, text.encode())
            return
        suffix = self.path.removeprefix("/page/")
        if not self.path.startswith("/page/") or not suffix.isascii() or not suffix.isdigit():
            self.send_body(404, b"unknown page\n")
            return
        page = int(suffix)
        if str(page) != suffix or page >= workload.pages:
            self.send_body(404, b"unknown page\n")
            return
        with server.lock:
            server.counts[page] += 1
            server.active += 1
            server.peak = max(server.peak, server.active)
        try:
            time.sleep(workload.delay_ms / 1000)
            body = workload.body(page)
        finally:
            # End server work before dispatch: handler teardown may overlap a client's
            # replacement request after it has consumed the final response byte.
            with server.lock:
                server.active -= 1
        self.send_body(200, body, "text/html")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument("--pages", type=int)
    parser.add_argument("--fanout", type=int)
    parser.add_argument("--delay-ms", type=int)
    parser.add_argument("--body-bytes", type=int)
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error("port must be 0..65535")
    try:
        overrides = {field: getattr(args, field) for field in ("pages", "fanout", "delay_ms", "body_bytes")}
        workload = scenario_workload(args.scenario, **overrides) if args.scenario else Workload(
            **{key: value for key, value in overrides.items() if value is not None})
    except ValueError as error:
        parser.error(str(error))
    with BenchmarkServer(args.port, workload) as server:
        print(f"http://127.0.0.1:{server.server_port}/page/0", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
