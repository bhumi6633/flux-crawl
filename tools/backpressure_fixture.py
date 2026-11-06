"""Deterministic branching graph for queue-pressure measurements and tests."""
from multi_origin import MultiOrigin

class FanoutFixture(MultiOrigin):
    def __init__(self, branches=20, leaves=500, delay=0, **kwargs):
        super().__init__([branches] * 3, [delay] * 3, **kwargs)
        self.branches, self.leaves = branches, leaves

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
            links = [((b + n) % 3, f'/leaf/{b}/{n}') for n in range(self.leaves)]
        else:
            links = []
        body = ('<html>' + ''.join(f'<a href="{self.url(h,p)}">link</a>' for h,p in links) + '</html>').encode()
        return 200, {'Content-Type': 'text/html'}, body
