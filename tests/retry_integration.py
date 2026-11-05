"""Local retry lifecycle, timing, accounting, politeness and stress verification."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from multi_origin import MultiOrigin

DRIVER = sys.argv[1]
FIELDS = "attempted discovered pending scheduled retried exhausted recovered retry_after cancelled completed http_failures fetch_failures".split()


def crawl(fixture, engine, path="/", budget=10000, retries=2, backoff=30, cap=2, rate=0, jitter=False, concurrency=8):
    result = subprocess.run([DRIVER, fixture.url(path=path), engine, str(budget), str(retries), str(backoff),
                             str(cap), str(rate), str(int(jitter)), str(concurrency)],
                            text=True, capture_output=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [line.split("\t") for line in result.stdout.splitlines()]
    summary = dict(zip(FIELDS, map(int, rows[-1][1:]), strict=True))
    return rows[:-1], summary


def starts(fixture, origin, path):
    return [t for h, p, t in fixture.snapshot()["starts"] if h == origin and p == path]


def standard():
    for engine in ("sequential", "threaded", "async"):
        with MultiOrigin((8, 8), [0.002, 0.002], graph="retry") as fixture:
            records, summary = crawl(fixture, engine, backoff=80)
            assert summary == dict(zip(FIELDS, [22, 14, 0, 8, 8, 1, 5, 1, 0, 14, 2, 1])), summary
            counts = fixture.snapshot()["counts"]
            assert counts[(0, "/flaky-503")] == 3 and counts[(0, "/always-503")] == 3
            for path in ("/not-found", "/large", "/healthy", "/redirect"):
                assert counts[(0, path)] == 1
            assert next(row for row in records if row[0] == fixture.url(path="/flaky-429"))[3] == "1"
            assert all(row[3] == "-" for row in records if row[0] == fixture.url(path="/redirect"))
            timed = starts(fixture, 0, "/flaky-503")
            assert timed[1] - timed[0] >= 0.05 and timed[2] - timed[1] >= 0.12, timed
            timed = starts(fixture, 0, "/flaky-429")
            assert timed[1] - timed[0] >= 0.85, timed
            assert starts(fixture, 0, "/healthy")[0] < starts(fixture, 0, "/flaky-503")[1]
            assert starts(fixture, 1, "/healthy/0")[0] < starts(fixture, 0, "/flaky-503")[1]
            stats = fixture.snapshot()
            assert max(stats["peak_per_origin"]) <= 2 and stats["global_peak"] <= (1 if engine == "sequential" else 8)
            print(engine, "mixed retry summary", summary)
        for path in ("/json", "/broken"):
            with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
                _, summary = crawl(fixture, engine, path=path)
                assert summary["attempted"] == 1 and summary["scheduled"] == 0
        with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
            _, summary = crawl(fixture, engine, path="/huge-after", budget=1)
            assert summary["retry_after"] == 1 and summary["cancelled"] == 1
        # No active or ready work: future retries must keep the crawl alive.
        with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
            _, summary = crawl(fixture, engine, path="/flaky-503")
            assert summary["attempted"] == 3 and summary["discovered"] == 1 and summary["recovered"] == 1
        # Deadline expires while a transfer still runs: event loop must launch retry before it finishes.
        with MultiOrigin((1, 1), [0, 0.25], graph="retry") as fixture:
            if engine != "sequential":
                _, summary = crawl(fixture, engine, backoff=20)
                assert starts(fixture, 0, "/flaky-503")[1] < starts(fixture, 1, "/healthy/0")[0] + 0.23
        with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
            _, summary = crawl(fixture, engine, path="/flaky-503", backoff=5, rate=10)
            timed = starts(fixture, 0, "/flaky-503")
            assert all(b - a >= 0.07 for a, b in zip(timed, timed[1:])), timed
            assert summary["recovered"] == 1
        for budget in (0, 1, 2, 3):
            with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
                _, summary = crawl(fixture, engine, path="/always-503", budget=budget)
                assert summary["attempted"] == budget and summary["discovered"] == 1
                assert summary["cancelled"] == (1 if budget in (1, 2) else 0), summary
                assert summary["exhausted"] == (1 if budget == 3 else 0)
        with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
            _, summary = crawl(fixture, engine, path="/always-503", retries=0)
            assert summary["attempted"] == 1 and summary["scheduled"] == 0 and summary["exhausted"] == 0


def stress():
    for engine in ("threaded", "async"):
        for budget in (10000, 211):
            with MultiOrigin([50] * 4, [0.003] * 4, graph="retry-stress") as fixture:
                _, summary = crawl(fixture, engine, budget=budget, backoff=10, cap=3, jitter=True, concurrency=16)
                stats = fixture.snapshot()
                assert summary["discovered"] == 201
                assert summary["attempted"] == min(291, budget)
                assert max(stats["peak_per_origin"]) <= 3 and stats["global_peak"] <= 16
                assert stats["global_peak"] > 3
                assert max(stats["counts"].values()) <= 3
                if budget == 10000:
                    assert summary["scheduled"] == summary["retried"] == 90
                    assert summary["recovered"] == 50 and summary["exhausted"] == 20
                    assert summary["pending"] == 0 and summary["completed"] == 201
                    expected = {(0, "/"): 1} | {(h, f"/page/{p}"): 2 if h * 50 + p < 50 else 3 if h * 50 + p < 70 else 1
                                                       for h in range(4) for p in range(50)}
                    assert stats["counts"] == expected
                else:
                    assert summary["cancelled"] == summary["scheduled"] - summary["retried"]
                order = stats["starts"]
                assert next(i for i, (h, _, _) in enumerate(order) if h == 3) < 50
                print(engine, "retry stress", summary, "peaks", stats["peak_per_origin"], stats["global_peak"])


if __name__ == "__main__":
    stress() if len(sys.argv) > 2 and sys.argv[2] == "--stress" else standard()
