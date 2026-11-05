"""Per-origin robots modes and graphs; all requests use shared fixture instrumentation."""
import time
from multi_origin import MultiOrigin

PREFIX = b"User-agent: *\nDisallow: /private/\nAllow: /private/public/\n"
SPECIFIC = b"User-agent: *\nDisallow: /\nUser-agent: FluxCrawl\nDisallow: /private/\nAllow: /private/public/\n"


class RobotsFixture(MultiOrigin):
    def __init__(self, modes=("allow", "specific", "missing", "transient"), pages=None, graph="small"):
        super().__init__(pages or [100] * len(modes), [0.003] * len(modes), graph=graph)
        self.modes = modes

    def close_request(self, origin, path):
        return path == "/robots.txt" and self.modes[origin] == "close"

    def response(self, origin, path):
        with self.lock:
            attempt = self.counts[(origin, path)]
        if path == "/robots.txt":
            mode = self.modes[origin]
            if mode == "slow":
                time.sleep(0.4)
            if mode in ("missing", "gone", "forbid", "auth", "always503"):
                return {"missing": 404, "gone": 410, "forbid": 403, "auth": 401, "always503": 503}[mode], {}, b""
            if mode == "transient" and attempt == 1:
                return 503, {"Retry-After": "0"}, b""
            if mode == "large":
                return 200, {}, b"x" * (512 * 1024 + 1)
            if mode == "redirect":
                return 302, {"Location": self.url(origin, "/policy.txt")}, b""
            if mode == "root":
                return 200, {}, b"User-agent: *\nDisallow: /\n"
            if mode in ("prefix", "transient"):
                return 200, {}, PREFIX
            if mode == "specific":
                return 200, {}, SPECIFIC
            return 200, {}, b"User-agent: *\nAllow: /\n"
        if path == "/policy.txt":
            return 200, {}, PREFIX
        if path == "/resource-flaky" and attempt == 1:
            return 503, {}, b""
        if path == "/":
            if self.graph == "stress":
                links = [(h, f"/private/{p}" if p % 5 == 0 else f"/private/public/{p}" if p % 5 == 1 else f"/page/{p}")
                         for h, count in enumerate(self.pages) for p in range(count)]
                links += [(h, "/resource-flaky") for h in range(len(self.modes))]
            elif self.graph == "race":
                links = [(0, f"/spawn/{p}") for p in range(24)]
            else:
                links = [(h, p) for h in range(len(self.modes)) for p in ("/public", "/private/secret", "/private/public/info", "/robots.txt")]
        elif self.graph == "race" and path.startswith("/spawn/"):
            links = [(1, f"/page/{p}") for p in range(100)]
        else:
            links = [(0, "/")]  # No new resources beyond depth two.
        body = ("<html>" + "".join(f'<a href="{self.url(h, p)}">link</a>' for h, p in links) + "</html>").encode()
        return 200, {"Content-Type": "text/html"}, body
