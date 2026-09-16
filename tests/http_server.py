"""Run one C++ integration test against a loopback-only HTTP fixture."""

import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import socket
import subprocess
import sys
import time
import threading
from collections import Counter


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = False
    request_queue_size = 128


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.requests.append(self.path)
        # Count outstanding server work until response dispatch; completed handlers
        # may still be unwinding after the client has already started replacement work.
        self.activity_data = None
        tracked = self.path.startswith(("/stress/", "/refill/")) and not self.path.endswith("/stats")
        key = self.path.rsplit("/", 1)[0]
        if tracked:
            with self.server.lock:
                data = self.server.activity.setdefault(key, {"active": 0, "peak": 0, "counts": Counter()})
                data["active"] += 1
                data["peak"] = max(data["peak"], data["active"])
                data["counts"][self.path.rsplit("/", 1)[1]] += 1
                self.activity_data = data
        try:
            self.respond()
        finally:
            self.finish_activity()

    def finish_activity(self):
        if self.activity_data is not None:
            with self.server.lock:
                self.activity_data["active"] -= 1
                self.activity_data = None

    def respond(self):
        status, body, content_type = 200, b"hello", "text/plain"
        headers = {}
        if self.path.startswith(("/stress/", "/refill/")) and self.path.endswith("/stats"):
            with self.server.lock:
                data = self.server.activity[self.path.rsplit("/", 1)[0]]
                body = f"{data['peak']} {sum(data['counts'].values())} {max(data['counts'].values())}".encode()
        elif self.path.startswith("/refill/"):
            prefix, page = self.path.rsplit("/", 1)
            with self.server.lock:
                data = self.server.runs.setdefault(prefix, {"a": threading.Event(), "c": threading.Event(),
                                                          "d": threading.Event(), "passed": False})
            if page == "root":
                links = ["a", "b", "c", "d"]
            else:
                links = []
                if page in ("a", "c"):
                    data[page].set()
                    data["d"].wait(3)  # A batched engine times out and fails the check below.
                    data[page].clear()
                elif page == "b":
                    data["a"].wait(3)
                    data["c"].wait(3)
                elif page == "d":
                    data["passed"] = data["a"].is_set() and data["c"].is_set()
                    data["d"].set()
                elif page == "result":
                    body = b"1" if data["passed"] else b"0"
            if page != "result":
                body = "".join(f"<a href='{prefix}/{link}'>L</a>" for link in links).encode()
                content_type = "text/html"
        elif self.path.startswith("/pool/"):
            run, page = self.path[len("/pool/"):].split("/", 1)
            with self.server.lock:
                data = self.server.runs.setdefault(run, {"active": 0, "peak": 0, "counts": Counter(), "overlap": threading.Event()})
                if page == "stats":
                    body = f"{data['peak']} {data['counts']['shared']}".encode()
                else:
                    data["counts"][page] += 1
                    data["active"] += 1
                    data["peak"] = max(data["peak"], data["active"])
                    if data["active"] > 1:
                        data["overlap"].set()
            if page != "stats":
                try:
                    prefix = f"/pool/{run}/"
                    if page == "root":
                        time.sleep(0.02)  # Only seed active; other workers must wait, not exit.
                        links = ["a", "b", "c", "d"]
                    elif page in ("a", "b", "c", "d"):
                        if run.startswith("overlap4"):
                            data["overlap"].wait(2)
                        time.sleep(0.03)
                        links = ["shared", "root"]
                    else:
                        links = []
                    body = "".join(f"<a href='{prefix}{link}'>L</a>" for link in links).encode()
                    content_type = "text/html"
                finally:
                    with self.server.lock:
                        data["active"] -= 1
        elif self.path.startswith("/stress/"):
            prefix, page = self.path.rsplit("/", 1)
            if page != "root":
                time.sleep(0.005)
            links = [str(i) for i in range(100)] if page == "root" else ["shared", "root"]
            if page == "shared":
                links = []
            body = "".join(f"<a href='{prefix}/{link}'>L</a>" for link in links).encode()
            content_type = "text/html"
        elif self.path.startswith("/depth-race/"):
            page = self.path.rsplit("/", 1)[1]
            graph = {"root": ["fast", "slow"], "fast": ["middle"], "middle": ["shared"],
                     "slow": ["shared"], "shared": ["leaf"], "leaf": []}
            if page == "slow":
                time.sleep(0.1)
            body = "".join(f"<a href='/depth-race/{link}'>L</a>" for link in graph[page]).encode()
            content_type = "text/html"
        elif self.path.startswith("/crawl/"):
            content_type = "text/html; charset=UTF-8"
            graph = {
                "/crawl/": ["/crawl/a", "/crawl/b", "/crawl/shared", "/crawl/redirect-start"],
                "/crawl/a": ["/crawl/deep", "/crawl/shared#one"],
                "/crawl/b": ["/crawl/shared#two", "/crawl/"],
                "/crawl/shared": [], "/crawl/deep": [],
                "/crawl/redirected/index.html": ["child.html"],
                "/crawl/redirected/child.html": [],
                "/crawl/failures": [self.server.unavailable, "/crawl/not-found", "/crawl/error",
                                    "/crawl/plain", "/crawl/binary", "/crawl/no-type", "/crawl/malformed"],
                "/crawl/after": [],
                "/crawl/isolated": ["/ok", "/empty", "/large", "/html"],
                "/crawl/transport": ["/ok", "/truncated", "/empty", "/html"],
                "/crawl/chain": ["/crawl/slow-chain"],
                "/crawl/slow-chain": ["/crawl/after"],
                "/crawl/host-policy": [f"http://localhost:{self.server.server_port}/crawl/shared",
                                       self.server.unavailable, "/crawl/shared"],
            }
            if self.path == "/crawl/slow-chain":
                time.sleep(0.05)
            if self.path in graph:
                body = "".join(f"<a href='{link}'>Link</a>" for link in graph[self.path]).encode()
            elif self.path == "/crawl/redirect-start":
                status, body = 302, b"redirect"
                headers["Location"] = "/crawl/redirected/index.html"
            elif self.path == "/crawl/malformed":
                body = b"<html><body><a href='/crawl/after'>After"
            else:
                body = b"<a href='/crawl/evil'>Must not discover</a>"
                if self.path == "/crawl/not-found":
                    status = 404
                elif self.path == "/crawl/error":
                    status = 503
                elif self.path == "/crawl/plain":
                    content_type = "text/plain"
                elif self.path == "/crawl/binary":
                    content_type = "application/octet-stream"
                elif self.path == "/crawl/no-type":
                    content_type = None
                else:
                    status = 404
        elif self.path == "/html":
            body, content_type = b"<a href='next'>Next</a>", "text/html; charset=UTF-8"
        elif self.path == "/not-found":
            status, body = 404, b"not found"
        elif self.path in ("/http-500", "/http-429"):
            status = int(self.path.rsplit("-", 1)[1])
        elif self.path == "/server-error":
            status, body = 503, b"unavailable"
        elif self.path in ("/redirect", "/loop", "/forbidden", "/invalid-effective"):
            status, body = 302, b"intermediate body must not be returned"
            headers["Location"] = {
                "/redirect": "/html", "/loop": "/loop",
                "/forbidden": "file:///fluxcrawl-test-must-not-read",
                "/invalid-effective": "/%GG",
            }[self.path]
        elif self.path == "/binary":
            body, content_type = bytes([0, 1, 255, 65, 0, 66]), "application/octet-stream"
        elif self.path == "/no-content-type":
            content_type = None
        elif self.path == "/large":
            body = b"x" * 65536
        elif self.path == "/gzip":
            assert "gzip" in self.headers.get("Accept-Encoding", "")
            body = gzip.compress(b"decoded\x00body" * 1024, mtime=0)
            headers["Content-Encoding"] = "gzip"
        elif self.path == "/agent":
            body = self.headers.get("User-Agent", "").encode()
        elif self.path == "/empty":
            status, body = 204, b""
        elif self.path == "/slow":
            time.sleep(0.4)
        elif self.path == "/truncated":
            headers["Content-Length"] = "100"
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for chunk in (b"abc", b"\x00def", b"ghi"):
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            return
        self.finish_activity()
        self.send_response(status)
        if content_type is not None:
            self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", headers.pop("Content-Length", str(len(body))))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when the client aborts on timeout or body limit.


def main():
    # Hold a bound, non-listening socket so the refusal port cannot be stolen.
    with socket.socket() as refused, FixtureServer(("127.0.0.1", 0), Handler) as server:
        refused.bind(("127.0.0.1", 0))
        server.requests = []
        server.lock = threading.Lock()
        server.runs = {}
        server.activity = {}
        server.unavailable = f"http://127.0.0.1:{refused.getsockname()[1]}/"
        server.timeout = 0.05
        environment = {key: value for key, value in os.environ.items()
                       if key.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}
        environment["NO_PROXY"] = "*"
        command = [sys.argv[1], f"http://127.0.0.1:{server.server_port}",
                   f"http://127.0.0.1:{refused.getsockname()[1]}"]
        threaded_cli = len(sys.argv) > 2 and sys.argv[2] == "--threaded-cli"
        async_cli = len(sys.argv) > 2 and sys.argv[2] == "--async-cli"
        cli_mode = len(sys.argv) > 2 and sys.argv[2] in ("--crawl-cli", "--threaded-cli", "--async-cli")
        if cli_mode:
            command = [sys.argv[1], "--engine", "async" if async_cli else "threaded" if threaded_cli else "sequential",
                       "--depth", "3", "--max-pages", "50",
                       f"http://127.0.0.1:{server.server_port}/crawl/"]
            if threaded_cli:
                command += ["--workers", "4"]
            if async_cli:
                command += ["--concurrency", "8"]
            print("Running:", " ".join(command), flush=True)
        with subprocess.Popen(command, env=environment, stdout=subprocess.PIPE if cli_mode else None,
                              text=True) as child:
            try:
                deadline = time.monotonic() + 80
                while child.poll() is None:
                    if time.monotonic() > deadline:
                        raise TimeoutError("HTTP integration test exceeded 80 seconds")
                    server.handle_request()
                if cli_mode:
                    output = child.communicate()[0]
                    print(output, end="")
                    for line in ("Pages attempted: 7\n", "Responses: 7\n", "Fetch failures: 0\n",
                                 "URLs discovered: 7\n", "URLs pending: 0\n", "HTML pages processed: 7\n"):
                        assert line in output, output
                    expected = ["/crawl/", "/crawl/a", "/crawl/b", "/crawl/shared",
                                "/crawl/redirect-start", "/crawl/redirected/index.html",
                                "/crawl/deep", "/crawl/redirected/child.html"]
                    with server.lock:
                        actual = list(server.requests)
                    if async_cli:
                        assert "Engine: async\nConcurrency: 8\n" in output
                        assert Counter(actual) == Counter(expected), actual
                    elif threaded_cli:
                        assert "Engine: threaded\nWorkers: 4\n" in output
                        assert Counter(actual) == Counter(expected), actual
                    else:
                        assert "Engine: sequential\n" in output
                        assert actual == expected, actual
                with server.lock:
                    assert "/never-request-zero" not in server.requests
                return child.returncode
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait()


if __name__ == "__main__":
    sys.exit(main())
