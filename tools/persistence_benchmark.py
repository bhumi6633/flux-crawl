"""Dedicated whole-client persistence experiment; never mixed with baseline engine benchmarks."""
import argparse
import csv
from pathlib import Path
import socket
import sqlite3
from contextlib import closing
import statistics
import subprocess
import tempfile
import time
from multi_origin import MultiOrigin, Handler


class BenchmarkHandler(Handler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


def trial(binary, engine, batch, pages, concurrency, delay_ms):
    with tempfile.TemporaryDirectory() as tmp, MultiOrigin([pages] * 4, [delay_ms / 1000] * 4) as site:
        for server in site.servers:
            server.RequestHandlerClass = BenchmarkHandler
        expected = 1 + pages * 4
        command = [str(binary), "--engine", engine, "--workers" if engine == "threaded" else "--concurrency", str(concurrency),
                   "--unbounded-pending", "--depth", "1", "--max-pages", str(expected), "--max-per-host", "4", "--ignore-robots"]
        db = Path(tmp) / "crawl.db"
        if batch is not None:
            command += ["--state-db", str(db), "--db-batch-size", str(batch)]
        command += [site.url()]
        began = time.monotonic()
        result = subprocess.run(command, text=True, capture_output=True, timeout=120)
        elapsed = time.monotonic() - began
        if result.returncode != 0:
            raise RuntimeError(result.stdout + result.stderr)
        summary = dict(line.split(": ", 1) for line in result.stdout.splitlines() if ": " in line)
        assert int(summary["Pages attempted"]) == expected and int(summary["URLs pending"]) == 0
        stats = site.snapshot()
        assert set(stats["counts"]) == {(0, "/")} | {(h, f"/page/{p}") for h in range(4) for p in range(pages)}
        assert all(n == 1 for n in stats["counts"].values())
        assert max(stats["peak_per_origin"]) <= 4 and stats["global_peak"] <= concurrency
        if batch is not None:
            with closing(sqlite3.connect(f"file:{db}?mode=ro", uri=True)) as connection:
                assert connection.execute("SELECT COUNT(*) FROM seen_urls").fetchone()[0] == expected
                assert connection.execute("SELECT COUNT(*) FROM unfinished").fetchone()[0] == 0
                payload = connection.execute("SELECT value FROM crawl_meta WHERE key='payload'").fetchone()[0]
                assert "completed 1\n" in payload
        return {
            "persistence_mode": "none" if batch is None else "sqlite", "batch_size": batch or "", "engine": engine,
            "backpressure_mode": "disabled", "origins": 4, "pages_per_origin": pages, "global_concurrency": concurrency, "max_per_host": 4,
            "delay_ms": delay_ms, "requests": expected, "process_elapsed_ms": elapsed * 1000,
            "process_requests_per_second": expected / elapsed, "transactions": int(summary.get("SQLite transactions", 0)),
            "mutations": int(summary.get("SQLite mutations", 0)), "average_batch_mutations": float(summary.get("SQLite average batch mutations", 0)),
            "commit_ms": float(summary.get("SQLite commit time ms", 0)), "max_commit_ms": float(summary.get("SQLite max commit ms", 0)),
            "initialization_ms": float(summary.get("SQLite initialization ms", 0)), "database_bytes_closed": db.stat().st_size if batch is not None else 0,
            "global_peak": stats["global_peak"],
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build-release/fluxcrawl"))
    parser.add_argument("--engine", choices=("threaded", "async", "all"), default="async")
    parser.add_argument("--pages-per-origin", type=int, default=100)
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--delay-ms", type=float, default=0)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or min(args.pages_per_origin, args.concurrency, args.runs) <= 0 or args.warmups < 0 or args.delay_ms < 0:
        parser.error("positive workload/runs, nonnegative warmups/delay, and a fresh output path are required")
    engines = ("threaded", "async") if args.engine == "all" else (args.engine,)
    rows = []
    for run in range(-args.warmups, args.runs):
        for engine in engines:
            for batch in (None, 1, 10, 100, 500):
                row = trial(args.binary.resolve(), engine, batch, args.pages_per_origin, args.concurrency, args.delay_ms)
                if run >= 0:
                    row["run"] = run + 1; rows.append(row)
                    print(f"{engine} mode={row['persistence_mode']} batch={batch}: {row['process_elapsed_ms']:.3f}ms transactions={row['transactions']} commit={row['commit_ms']:.3f}ms")
    with args.output.open("x", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    for engine in engines:
        for batch in ("", 1, 10, 100, 500):
            selected = [r for r in rows if r["engine"] == engine and r["batch_size"] == batch]
            print(engine, "batch", batch or "none", {key: round(statistics.median(r[key] for r in selected), 3) for key in ("process_elapsed_ms", "process_requests_per_second", "transactions", "average_batch_mutations", "commit_ms", "database_bytes_closed")})


if __name__ == "__main__":
    main()
