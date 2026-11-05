"""Deterministic loopback origins with shared request-start/activity instrumentation.

Counters cover handler work until response dispatch, excluding socket cleanup.
The test graph lists an entire origin before the next to expose FIFO starvation.
"""
from collections import Counter
from contextlib import AbstractContextManager
from http.server import BaseHTTPRequestHandler
import threading
import time

from local_http import LocalHttpServer


class MultiOrigin(AbstractContextManager):
    def __init__(self, pages=(8, 8, 8, 8), delays=None, graph="flat"):
        self.pages = tuple(pages)
        self.delays = tuple(delays or [0.03] * len(pages))
        self.graph = graph
        self.lock = threading.Lock()
        self.servers = []
        self.threads = []
        self.active = [0] * len(pages)
        self.peak = [0] * len(pages)
        self.global_peak = 0
        self.starts = []
        self.counts = Counter()
        self.user_agents = []

    def __enter__(self):
        try:
            for origin in range(len(self.pages)):
                server = LocalHttpServer(("127.0.0.1", 0), Handler)
                server.fixture, server.origin = self, origin
                self.servers.append(server)
                thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
                thread.start()
                self.threads.append(thread)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        for server in self.servers:
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join()

    def url(self, origin=0, path="/"):
        return f"http://127.0.0.1:{self.servers[origin].server_port}{path}"

    def snapshot(self):
        with self.lock:
            return {"peak_per_origin": self.peak[:], "global_peak": self.global_peak,
                    "starts": self.starts[:], "user_agents": self.user_agents[:], "counts": dict(self.counts), "active": self.active[:]}

    def response(self, origin, path):
        status, headers = 200, {}
        if self.graph.startswith("retry"):
            with self.lock:
                attempt = self.counts[(origin, path)]
            if self.graph == "retry-stress":
                if path == "/":
                    links = [(i, f"/page/{p}") for i, count in enumerate(self.pages) for p in range(count)]
                else:
                    links = []
                    number = origin * self.pages[origin] + int(path.rsplit("/", 1)[1])
                    if number < 50 and attempt == 1 or 50 <= number < 70:
                        status = 503
            elif path == "/":
                links = [(0, p) for p in ("/flaky-429", "/flaky-503", "/always-503", "/not-found",
                         "/close", "/large", "/healthy", "/redirect", "/malformed", "/date")]
                links += [(1, f"/healthy/{p}") for p in range(3)]
            else:
                links = []
                if path == "/flaky-429" and attempt == 1:
                    status, headers = 429, {"rEtRy-AfTeR": "1"}
                if path == "/flaky-503" and attempt <= 2 or path == "/always-503":
                    status = 503
                if path == "/huge-after":
                    return 503, {"Retry-After": "9223372036854775807"}, b""
                if path == "/json":
                    return 200, {"Content-Type": "application/json"}, b"{}"
                if path == "/broken":
                    return 200, {}, b"<html><p><b>unfinished"
                if path == "/not-found":
                    status = 404
                if path == "/large":
                    return 200, {}, b"x" * 262144
                if path == "/redirect":
                    return 302, {"Location": self.url(1, "/destination"), "Retry-After": "2"}, b""
                if path in ("/malformed", "/date") and attempt == 1:
                    status = 503
                    headers = {"Retry-After": "junk" if path == "/malformed" else "Wed, 21 Oct 2015 07:28:00 GMT"}
            body = ("<html>" + "".join(f'<a href="{self.url(i, p)}">link</a>' for i, p in links) + "</html>").encode()
            return status, headers, body
        if self.graph == "depth":
            graph = {
                "/": [(0, "/a"), (1, "/b")],
                "/a": [(0, "/target")], "/b": [(1, "/mid")],
                "/mid": [(0, "/target")], "/target": [(0, "/tail")], "/tail": [],
            }
            links = graph.get(path, [])
        elif self.graph == "failures":
            links = [(0, p) for p in ("/404", "/500", "/close", "/large", "/redirect", "/last")] if path == "/" else []
            if path in ("/404", "/500"):
                status = int(path[1:])
            if path == "/large":
                return 200, {}, b"x" * 262144
            if path == "/redirect":
                return 302, {"Location": self.url(1, "/destination")}, b""
        else:
            links = ([(i, f"/page/{p}") for i, count in enumerate(self.pages) for p in range(count)]
                     if path == "/" else [(0, "/"), (origin, "/page/0")])
        body = ("<!doctype html><html>" + "".join(
            f'<a href="{self.url(i, p)}">link</a>' for i, p in links) + "</html>").encode()
        return status, headers, body


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        fixture, origin = self.server.fixture, self.server.origin
        with fixture.lock:
            fixture.active[origin] += 1
            fixture.peak[origin] = max(fixture.peak[origin], fixture.active[origin])
            fixture.global_peak = max(fixture.global_peak, sum(fixture.active))
            fixture.starts.append((origin, self.path, time.monotonic()))
            fixture.user_agents.append(self.headers.get("User-Agent", ""))
            fixture.counts[(origin, self.path)] += 1
        active = True
        try:
            time.sleep(fixture.delays[origin])
            with fixture.lock:
                attempt = fixture.counts[(origin, self.path)]
            if getattr(fixture, "close_request", lambda *_: False)(origin, self.path):
                self.close_connection = True
                return
            if self.path == "/close" and (fixture.graph == "failures" or fixture.graph == "retry" and attempt == 1):
                self.close_connection = True
                return
            status, headers, body = fixture.response(origin, self.path)
            # Complete tracked server work before dispatch, as in benchmark_server.
            with fixture.lock:
                fixture.active[origin] -= 1
                active = False
            self.send_response(status)
            self.send_header("Content-Type", headers.get("Content-Type", "text/html"))
            self.send_header("Content-Length", str(len(body)))
            for key, value in headers.items():
                if key != "Content-Type":
                    self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected for body-limit tests.
        finally:
            if active:
                with fixture.lock:
                    fixture.active[origin] -= 1
