"""Small deterministic recovery graph, with backlinks and optional transient resource."""
from multi_origin import MultiOrigin


class CheckpointFixture(MultiOrigin):
    def __init__(self, graph="tree"):
        super().__init__([3] * 3, [0.025] * 3, graph=graph)

    def response(self, origin, path):
        with self.lock:
            attempt = self.counts[(origin, path)]
        if path == "/robots.txt":
            return 200, {"Content-Type": "text/plain"}, b"User-agent: *\nDisallow: /private/\n"
        if self.graph == "retry":
            if path == "/":
                links = [(0, "/flaky"), (1, "/healthy")]
            elif path == "/flaky":
                if attempt < 3:
                    return 503, {}, b"temporary"
                links = [(0, "/tail")]
            elif path == "/healthy":
                links = [(0, "/"), (1, "/done")]
            else:
                links = []
        else:
            if path == "/":
                links = [(h, "/first") for h in range(3)] + [(0, "/private/blocked")]
            elif path == "/first":
                links = [(0, "/"), (origin, "/branch")]
            elif path == "/branch":
                links = [(origin, "/leaf"), (0, "/")]
            else:
                links = []
        return 200, {"Content-Type": "text/html"}, ("<html>" + "".join(f'<a href="{self.url(h, p)}">link</a>' for h, p in links) + "</html>").encode()
