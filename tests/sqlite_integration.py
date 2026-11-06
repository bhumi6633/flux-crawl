"""SQLite-backed engine parity, clean/crash resume, buffering and moderate stress."""
from contextlib import closing
from pathlib import Path
import sqlite3
import signal
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from checkpoint_fixture import CheckpointFixture
from multi_origin import MultiOrigin

BINARY = sys.argv[1]


def command(*args):
    return subprocess.run([BINARY, *map(str, args)], text=True, capture_output=True, timeout=120)


def inspect(path):
    with closing(sqlite3.connect(f"file:{path}?mode=ro", uri=True)) as db:
        db.execute("BEGIN")
        schema = db.execute("PRAGMA user_version").fetchone()[0]
        payload = db.execute("SELECT value FROM crawl_meta WHERE key='payload'").fetchone()[0]
        meta = dict(line.split(" ", 1) for line in payload.splitlines())
        seen = {row[0] for row in db.execute("SELECT url FROM seen_urls")}
        work = list(db.execute("SELECT url,depth,retry_count,recovery,state,eligible_ms FROM unfinished ORDER BY queue_order,url"))
        assert not list(db.execute("PRAGMA foreign_key_check"))
        db.commit()
    assert schema == 1
    assert int(meta["urls_discovered"]) == len(seen) and int(meta["urls_pending"]) == len(work)
    return meta, seen, work


def start(site, db, engine="async", batch=100, stop=None, budget=100, extra=()):
    args = ["--engine", engine, "--state-db", db, "--db-batch-size", str(batch), "--depth", "3", "--max-pages", str(budget)]
    if engine != "sequential":
        args += ["--workers" if engine == "threaded" else "--concurrency", "16"]
    if stop is not None:
        args += ["--stop-after-attempts", str(stop)]
    result = command(*args, *extra, site.url())
    assert result.returncode == 0, result.stdout + result.stderr
    assert "SQLite persistence errors: 0" in result.stdout
    return result


def resume(db, engine="async", batch=100):
    args = ["resume", db, "--engine", engine, "--db-batch-size", str(batch)]
    if engine != "sequential":
        args += ["--workers" if engine == "threaded" else "--concurrency", "16"]
    result = command(*args)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def terminate_after(site, db, batch, predicate):
    site.delays = tuple(0.10 for _ in site.delays)
    process = subprocess.Popen([BINARY, "--engine", "threaded", "--workers", "8", "--state-db", str(db),
                                "--db-batch-size", str(batch), "--depth", "3", "--max-pages", "1000", site.url()],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and process.poll() is None:
            if db.exists():
                try:
                    metadata, seen, work = inspect(db)
                    if predicate(metadata, seen, work):
                        break
                except (sqlite3.Error, AssertionError, TypeError):
                    pass
            time.sleep(0.005)
        else:
            raise AssertionError("No committed recovery boundary observed")
        if hasattr(signal, "SIGSTOP"):
            process.send_signal(signal.SIGSTOP)  # Keep ownership stable while a second process starts.
            owner = command("resume", db)
            assert owner.returncode != 0 and "owned" in owner.stderr, owner.stdout + owner.stderr
            process.kill()
        else:
            process.terminate()
        _, errors = process.communicate(timeout=30)
        assert process.returncode != 0 and "WARNING: ThreadSanitizer" not in errors, errors
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=30)


def standard(directory):
    for flag in ("--db-batch-size",):
        for value in ("0", "-1", "x", "1.5", "184467440737095516160"):
            assert command(flag, value, "http://127.0.0.1:1/").returncode != 0
    for flags in (("--state-db",), ("--db-batch-size", "10"),
                  ("--state-db", directory / "conflict.db", "--checkpoint", directory / "conflict.chk")):
        assert command(*flags).returncode != 0
    assert command("resume", directory / "absent.db").returncode != 0
    for initial, restarted in (("sequential", "async"), ("threaded", "async"), ("async", "threaded")):
        with CheckpointFixture() as site:
            db = directory / f"{initial}.db"
            start(site, db, initial, batch=10, stop=3)
            meta, seen, work = inspect(db)
            assert int(meta["pages_attempted"]) == 3 and work
            before = site.snapshot()["counts"]
            collision = command("--state-db", db, site.url())
            assert collision.returncode != 0 and site.snapshot()["counts"] == before
            assert command("resume", db, "--depth", "1").returncode != 0
            assert command("resume", db, "--checkpoint", directory / "bad.chk").returncode != 0
            result = resume(db, restarted)
            meta, seen, work = inspect(db)
            assert int(meta["pages_attempted"]) == len(seen) == 11 and not work and meta["completed"] == "1"
            assert all(n == 1 for n in site.snapshot()["counts"].values())
            expected = {site.url()} | {site.url(h, p) for h in range(3) for p in ("/first", "/branch", "/leaf")} | {site.url(path="/private/blocked")}
            assert seen == expected
            again = command("resume", db)
            assert again.returncode == 0 and "already completed" in again.stdout
            print(initial, "->", restarted, "SQLite resume: 11 exact resources")
    with CheckpointFixture("retry") as site:
        db = directory / "retry.db"
        start(site, db, "sequential", batch=1, stop=4, extra=("--max-retries", "2", "--retry-backoff-ms", "200", "--retry-max-backoff-ms", "1000", "--no-retry-jitter"))
        meta, _, work = inspect(db)
        retry = next(w for w in work if w[0].endswith("/flaky"))
        assert retry[2] == 2 and retry[4] == 1 and retry[5] is not None
        resume(db)
        meta, seen, work = inspect(db)
        assert int(meta["pages_attempted"]) == 7 and int(meta["retries_attempted"]) == 2 and len(seen) == 5 and not work
        times = [t for h, p, t in site.snapshot()["starts"] if h == 0 and p == "/flaky"]
        assert times[2] - times[1] >= 0.30
    with CheckpointFixture() as site:
        db = directory / "robots.db"
        start(site, db, "sequential", stop=3, extra=("--respect-robots",))
        before = site.snapshot()["counts"]
        resume(db)
        meta, seen, work = inspect(db)
        assert len(seen) == 11 and not work and int(meta["urls_blocked_by_robots"]) == 1
        after = site.snapshot()["counts"]
        assert (0, "/private/blocked") not in after and after[(0, "/")] == 1
        assert sum(n for (_, p), n in after.items() if p == "/robots.txt") > sum(n for (_, p), n in before.items() if p == "/robots.txt")
    for restarted in ("threaded", "async"):
        with CheckpointFixture() as site:
            db = directory / f"budget-{restarted}.db"
            start(site, db, "sequential", stop=8, budget=10)
            resume(db, restarted)
            meta, _, work = inspect(db)
            assert int(meta["pages_attempted"]) == 10 and len(work) == 1
            assert "no remaining" in command("resume", db).stdout
    with CheckpointFixture() as site:
        db = directory / "crash.db"
        terminate_after(site, db, 1, lambda m, s, w: int(m["pages_attempted"]) >= 2 and any(x[4] == 2 for x in w))
        result = resume(db)
        meta, seen, work = inspect(db)
        assert len(seen) == 11 and not work and int(meta["recovery_attempts"]) > 0
        assert site.snapshot()["counts"][(0, "/")] == 1
        print("Crash SQLite resume:", result.stdout.strip().replace("\n", "; "))
    with MultiOrigin((100,), [0.001]) as site:
        db = directory / "buffered.db"
        terminate_after(site, db, 100000, lambda m, s, w: len(site.snapshot()["starts"]) >= 10)
        meta, seen, work = inspect(db)
        assert len(seen) == len(work) == 1 and int(meta["pages_attempted"]) == 0  # Only the initialization prefix is durable.
        resume(db)
        meta, seen, work = inspect(db)
        assert len(seen) == 101 and not work and site.snapshot()["counts"][(0, "/")] == 2
    with MultiOrigin((100,), [0]) as site:
        db = directory / "clean-buffer.db"
        result = start(site, db, batch=100000, budget=1000)
        meta, seen, work = inspect(db)
        assert len(seen) == 101 and not work and meta["completed"] == "1"
        assert "SQLite transactions: 1" in result.stdout
    print("SQLite engine, buffering, budget, retry, policy and crash integration passed")


def stress(directory):
    for engine in ("threaded", "async"):
        with MultiOrigin([500] * 4, [0.001] * 4) as site:
            db = directory / f"stress-{engine}.db"
            result = start(site, db, engine, batch=100, budget=3000, extra=("--max-per-host", "3"))
            meta, seen, work = inspect(db)
            stats = site.snapshot()
            assert len(seen) == 2001 and not work and int(meta["pages_attempted"]) == 2001 and meta["completed"] == "1"
            assert all(n == 1 for n in stats["counts"].values())
            assert max(stats["peak_per_origin"]) <= 3 and 3 < stats["global_peak"] <= 16
            print(engine, "SQLite 2001-resource stress: peaks", stats["peak_per_origin"], "global", stats["global_peak"])
            print(result.stdout.strip().split("Persistence mode:")[-1])


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        stress(Path(tmp)) if len(sys.argv) > 2 and sys.argv[2] == "--stress" else standard(Path(tmp))
