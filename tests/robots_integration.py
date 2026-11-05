"""Policy bootstrap, conservative fallback, origin fairness and stress verification."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from robots_fixture import RobotsFixture

DRIVER = sys.argv[1]
FIELDS = "attempted discovered pending fetches origins hits allowed blocked failures reused scheduled retried cancelled resource_retries recovered".split()


def crawl(site, engine, path="/", budget=10000, cap=2, rate=0, concurrency=8):
    result = subprocess.run([DRIVER, site.url(path=path), engine, str(budget), str(cap), str(rate), str(concurrency), "1"],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [line.split("\t") for line in result.stdout.splitlines()]
    summary = dict(zip(FIELDS, map(int, rows[-1][1:]), strict=True))
    assert all(agent.startswith("FluxCrawl/") for agent in site.snapshot()["user_agents"])
    stats = site.snapshot()
    assert max(stats["peak_per_origin"]) <= cap
    assert stats["global_peak"] <= (1 if engine == "sequential" else concurrency)
    return rows[:-1], summary


def standard():
    for engine in ("sequential", "threaded", "async"):
        with RobotsFixture() as site:
            records, summary = crawl(site, engine)
            assert summary["attempted"] == 11 and summary["discovered"] == 17 and summary["pending"] == 0, summary
            assert summary["fetches"] == 5 and summary["origins"] == 4 and summary["blocked"] == 2 and summary["reused"] == 4, summary
            assert summary["scheduled"] == summary["retried"] == 1 and summary["failures"] == 0
            stats = site.snapshot()
            for h in (1, 3):
                assert (h, "/private/secret") not in stats["counts"]
                assert stats["counts"][(h, "/private/public/info")] == 1
            assert stats["counts"][(0, "/robots.txt")] == 1 and stats["counts"][(3, "/robots.txt")] == 2
            assert all(int(row[2]) <= 1 for row in records if row[0] == "resource")
            print(engine, "mixed robots summary", summary)
        for mode in ("root", "forbid", "auth", "large", "always503", "close"):
            with RobotsFixture((mode,)) as site:
                _, summary = crawl(site, engine)
                expected = 3 if mode in ("always503", "close") else 1
                assert summary["attempted"] == 0 and summary["blocked"] == 1 and summary["pending"] == 0, summary
                assert summary["fetches"] == expected and summary["failures"] == (0 if mode == "root" else 1), summary
                assert all(p == "/robots.txt" for _, p, _ in site.snapshot()["starts"])
        for mode in ("missing", "gone", "redirect"):
            with RobotsFixture((mode,)) as site:
                _, summary = crawl(site, engine)
                assert summary["fetches"] == 1 and summary["origins"] == 1 and summary["failures"] == 0
                assert summary["attempted"] > 0 and summary["blocked"] == (1 if mode == "redirect" else 0), summary
        with RobotsFixture(("prefix",)) as site:
            _, summary = crawl(site, engine, path="/private/secret")
            assert summary["blocked"] == 1 and summary["attempted"] == 0
        with RobotsFixture(("allow",), graph="stress") as site:
            _, summary = crawl(site, engine)
            assert summary["fetches"] == summary["origins"] == 1 and summary["discovered"] == 102, summary
            assert summary["resource_retries"] == 1 and summary["recovered"] == 1
        with RobotsFixture(("allow",)) as site:
            _, summary = crawl(site, engine, rate=10, budget=1)
            times = site.snapshot()["starts"]
            assert times[0][1] == "/robots.txt" and times[1][1] == "/"
            assert times[1][2] - times[0][2] >= 0.07
        with RobotsFixture(("allow",)) as site:
            _, summary = crawl(site, engine, budget=0)
            assert summary["attempted"] == summary["fetches"] == 0 and summary["pending"] == 1
            assert not site.snapshot()["starts"]
        with RobotsFixture(("allow", "slow", "allow")) as site:
            if engine != "sequential":
                _, summary = crawl(site, engine)
                starts = site.snapshot()["starts"]
                slow_start = next(t for h, p, t in starts if h == 1 and p == "/robots.txt")
                healthy_start = next(t for h, p, t in starts if h == 2 and p == "/public")
                assert healthy_start < slow_start + 0.35, starts
        # Concurrent publishers repeatedly rediscover 100 URLs on a new origin.
        with RobotsFixture(("allow", "allow"), graph="race") as site:
            _, summary = crawl(site, engine, concurrency=16)
            assert summary["discovered"] == summary["attempted"] == 125, summary
            assert summary["fetches"] == 2 and site.snapshot()["counts"][(1, "/robots.txt")] == 1


def stress():
    modes = ("allow", "prefix", "missing", "transient", "specific", "allow", "missing", "prefix")
    for engine in ("threaded", "async"):
        for budget in (10000, 37):
            with RobotsFixture(modes, graph="stress") as site:
                _, summary = crawl(site, engine, cap=3, concurrency=16, budget=budget)
                assert summary["attempted"] <= budget and summary["attempted"] == (737 if budget == 10000 else 37), summary
                stats = site.snapshot()
                assert summary["discovered"] == 809
                for h in (1, 3, 4, 7):
                    assert not any(host == h and path.startswith("/private/") and not path.startswith("/private/public/")
                                   for host, path in stats["counts"])
                assert max(stats["counts"].values()) <= 2
                if budget == 10000:
                    assert summary["blocked"] == 80 and summary["pending"] == 0, summary
                    assert summary["fetches"] == 9 and summary["origins"] == 8 and summary["retried"] == 1
                    assert summary["resource_retries"] == summary["recovered"] == 8
                    for h in range(8):
                        assert stats["counts"][(h, "/robots.txt")] == (2 if h == 3 else 1)
                    assert stats["global_peak"] > 3
                print(engine, "robots stress", summary, "peaks", stats["peak_per_origin"], stats["global_peak"])


if __name__ == "__main__":
    stress() if len(sys.argv) > 2 and sys.argv[2] == "--stress" else standard()
