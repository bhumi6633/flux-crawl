"""Shared concurrent loopback server base for correctness and benchmark fixtures."""
from http.server import ThreadingHTTPServer


class LocalHttpServer(ThreadingHTTPServer):
    daemon_threads = False
    request_queue_size = 128
