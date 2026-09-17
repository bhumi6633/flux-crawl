import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from benchmark_server import Workload


class GraphTests(unittest.TestCase):
    def test_connected_exact_payload(self):
        for fanout in (1, 2, 4, 16):
            graph = Workload(50, fanout, 0, 1024)
            pending, seen = [0], set()
            while pending:
                page = pending.pop()
                self.assertNotIn(page, seen)
                seen.add(page)
                pending.extend(graph.children(page))
                body = graph.body(page)
                self.assertEqual(len(body), 1024)
                self.assertEqual(body, graph.body(page))
            self.assertEqual(seen, set(range(50)))

    def test_invalid(self):
        for args in ((0, 4, 0, 1024), (10, 0, 0, 1024), (10, 4, -1, 1024), (10, 4, 0, 1)):
            with self.assertRaises(ValueError):
                Workload(*args)


if __name__ == "__main__":
    unittest.main()
