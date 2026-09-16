"""Run one C++ integration test against a sequential, loopback-only HTTP fixture."""

import gzip
from http.server import BaseHTTPRequestHandler, HTTPServer
import os
import socket
import subprocess
import sys
import time


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        status, body, content_type = 200, b"hello", "text/plain"
        headers = {}
        if self.path == "/html":
            body, content_type = b"<a href='next'>Next</a>", "text/html; charset=UTF-8"
        elif self.path == "/not-found":
            status, body = 404, b"not found"
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
    with socket.socket() as refused, HTTPServer(("127.0.0.1", 0), Handler) as server:
        refused.bind(("127.0.0.1", 0))
        server.timeout = 0.05
        environment = {key: value for key, value in os.environ.items()
                       if key.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}
        environment["NO_PROXY"] = "*"
        command = [sys.argv[1], f"http://127.0.0.1:{server.server_port}",
                   f"http://127.0.0.1:{refused.getsockname()[1]}"]
        with subprocess.Popen(command, env=environment) as child:
            try:
                deadline = time.monotonic() + 25
                while child.poll() is None:
                    if time.monotonic() > deadline:
                        raise TimeoutError("HTTP integration test exceeded 25 seconds")
                    server.handle_request()
                return child.returncode
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait()


if __name__ == "__main__":
    sys.exit(main())
