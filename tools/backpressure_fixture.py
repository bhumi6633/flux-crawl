"""Deterministic branching graph for queue-pressure measurements and tests."""
import socket
from multi_origin import MultiOrigin, Handler

class FanoutHandler(Handler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

class FanoutFixture(MultiOrigin):
    def __init__(self, branches=20, leaves=500, delay=0, duplicates=False, retry_every=0, skewed=False, **kwargs):
        super().__init__([branches] * 3, [delay] * 3, **kwargs)
        self.branches, self.leaves = branches, leaves
        self.duplicates, self.retry_every, self.skewed = duplicates, retry_every, skewed

    def __enter__(self):
        super().__enter__()
        for server in self.servers:
            server.RequestHandlerClass = FanoutHandler
        return self

    @property
    def total(self):
        return 1 + self.branches + self.branches * self.leaves

    def response(self, origin, path):
        if path == '/robots.txt':
            return 200, {'Content-Type': 'text/plain'}, b'User-agent: *\nAllow: /\n'
        if path == '/':
            links = [(b % 3, f'/branch/{b}') for b in range(self.branches)]
        elif path.startswith('/branch/'):
            b = int(path.rsplit('/', 1)[1])
            links = [((b if self.skewed else b + n) % 3, f'/leaf/{b}/{n}') for n in range(self.leaves)]
        else:
            links = []
            if path.startswith('/leaf/') and self.retry_every:
                b,n = map(int,path.split('/')[-2:])
                with self.lock: attempt = self.counts[(origin,path)]
                if (b*self.leaves+n) % self.retry_every == 0 and attempt == 1:
                    return 503, {'Content-Type':'text/plain'}, b'retry'
        if self.duplicates:
            links = links + links + [(0,'/leaf/0/0')] if links else []
        body = ('<html>' + ''.join(f'<a href="{self.url(h,p)}">link</a>' for h,p in links) + '</html>').encode()
        return 200, {'Content-Type': 'text/html'}, body
