"""Host caps, fairness, delayed termination, failures, depth and stress checks."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from multi_origin import MultiOrigin

DRIVER = sys.argv[1]


def crawl(fixture, engine, cap=2, rate=0, burst=1, limit=10000, depth=3,
          global_limit=8, same_host=False, body_limit=1048576):
    result = subprocess.run([DRIVER, fixture.url(), engine, str(global_limit), str(cap), str(rate),
                             str(burst), str(limit), str(depth), str(int(same_host)), str(body_limit)],
                            capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    lines = [line.split("\t") for line in result.stdout.splitlines()]
    summary = list(map(int, lines[-1][1:]))
    records = {line[0]: (int(line[1]), line[2], line[3]) for line in lines[:-1]}
    assert len(records) == summary[0]
    return records, summary


def check_activity(fixture, expected, cap, global_limit):
    stats = fixture.snapshot()
    assert sum(stats["counts"].values()) == expected, stats
    assert all(n == 1 for n in stats["counts"].values()), stats
    assert max(stats["peak_per_origin"]) <= cap, stats
    assert stats["global_peak"] <= global_limit, stats
    return stats


def standard():
    for engine in ("sequential", "threaded", "async"):
        with MultiOrigin() as fixture:
            records, summary = crawl(fixture, engine)
            assert summary == [33, 0, 0], summary
            expected = {fixture.url(): 0} | {fixture.url(i, f"/page/{p}"): 1 for i in range(4) for p in range(8)}
            assert {u: row[0] for u, row in records.items()} == expected
            stats = check_activity(fixture, 33, 2, 1 if engine == "sequential" else 8)
            if engine != "sequential":
                assert stats["global_peak"] > 2, stats
            print(engine, "multi-origin peaks", stats["peak_per_origin"], "global", stats["global_peak"])
        # Temporary ineligibility with no active work; burst one also rate-limits the seed's host.
        with MultiOrigin((3, 3), [0, 0]) as fixture:
            records, summary = crawl(fixture, engine, rate=10, same_host=True)
            assert summary == [7, 0, 0], summary
            starts = fixture.snapshot()["starts"]
            for origin in range(2):
                times = [t for i, _, t in starts if i == origin]
                # Arrival timestamps are not exact client admission timestamps. Broad lower bound only.
                assert times[-1] - times[0] >= (len(times) - 1) * 0.07, times
            print(engine, "rate=10 burst=1 spans", [round(max(t for i, _, t in starts if i == h) - min(t for i, _, t in starts if i == h), 3) for h in range(2)])
        with MultiOrigin((1, 1), [0.03, 0.001], graph="depth") as fixture:
            records, summary = crawl(fixture, engine, cap=1)
            assert summary == [6, 0, 0], summary
            assert records[fixture.url(0, "/target")][0] == 2
            assert records[fixture.url(0, "/tail")][0] == 3
        with MultiOrigin((1, 1), [0, 0], graph="failures") as fixture:
            records, summary = crawl(fixture, engine, cap=1, body_limit=4096)
            assert summary == [7, 2, 0], (summary, records)
            assert records[fixture.url(0, "/404")][1] == "404"
            assert records[fixture.url(0, "/500")][1] == "500"
            assert records[fixture.url(0, "/redirect")][2] == fixture.url(1, "/destination")
            assert records[fixture.url(0, "/last")][1] == "200"
        # With limits disabled the complete logical set is unchanged.
        with MultiOrigin((3, 3), [0, 0]) as fixture:
            records, summary = crawl(fixture, engine, cap=0)
            assert summary == [7, 0, 0]
            assert set(records) == {fixture.url()} | {fixture.url(i, f"/page/{p}") for i in range(2) for p in range(3)}
        for limit in (0, 1, 5):
            with MultiOrigin((8, 8), [0, 0]) as fixture:
                _, summary = crawl(fixture, engine, limit=limit, rate=100)
                assert summary[0] == limit
                check_activity(fixture, limit, 2, 8)
    for engine in ("threaded", "async"):
        with MultiOrigin((16,), [0.04]) as fixture:
            _, summary = crawl(fixture, engine, cap=3, global_limit=16)
            assert summary == [17, 0, 0]
            stats = check_activity(fixture, 17, 3, 16)
            assert stats["global_peak"] == 3, stats
        with MultiOrigin((100, 2), [0.008, 0.001]) as fixture:
            _, summary = crawl(fixture, engine, cap=8)
            assert summary == [103, 0, 0]
            starts = fixture.snapshot()["starts"]
            assert next(index for index, (origin, _, _) in enumerate(starts) if origin == 1) < 100
            check_activity(fixture, 103, 8, 8)
            print(engine, "small host starts before large queue drains")


def stress():
    for engine in ("threaded", "async"):
        for limit in (10000, 37):
            with MultiOrigin([100] * 8, [0.005] * 8) as fixture:
                _, summary = crawl(fixture, engine, cap=4, global_limit=32, limit=limit)
                expected = min(limit, 801)
                assert summary == [expected, 0, 801 - expected], summary
                stats = check_activity(fixture, expected, 4, 32)
                assert stats["global_peak"] > 4
                print(engine, "stress", expected, "requests; peaks", stats["peak_per_origin"], "global", stats["global_peak"])


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[2] == "--stress":
        stress()
    else:
        standard()
