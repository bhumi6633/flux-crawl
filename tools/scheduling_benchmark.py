"""Optional, separately reported multi-origin scheduling scenario.

Uses the crawl CLI and reports process wall time (including startup), NOT the
internal elapsed/latency metrics of the original single-origin benchmark runner.
Every trial validates exact fixture request identities, host caps and rate envelope.
"""
import argparse
import csv
import math
from pathlib import Path
import statistics
import subprocess
import time

from multi_origin import MultiOrigin


def positive_int(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def positive_rate(value):
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return result


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def trial(binary, engine, concurrency, pages, delay_ms, cap=None, rate=None, burst=1):
    with MultiOrigin(pages, [delay_ms / 1000] * len(pages)) as fixture:
        expected = 1 + sum(pages)
        command = [str(binary), "--engine", engine, "--depth", "1", "--max-pages", str(expected)]
        if engine != "sequential":
            command += ["--workers" if engine == "threaded" else "--concurrency", str(concurrency)]
        if cap is not None:
            command += ["--max-per-host", str(cap)]
        if rate is not None:
            command += ["--rate-per-host", str(rate), "--burst-per-host", str(burst)]
        command += [fixture.url()]
        started = time.monotonic()
        result = subprocess.run(command, text=True, capture_output=True, timeout=300)
        elapsed = time.monotonic() - started
        if result.returncode != 0:
            raise RuntimeError(result.stdout + result.stderr)
        summary = dict(line.split(": ", 1) for line in result.stdout.splitlines() if ": " in line)
        require(int(summary["Pages attempted"]) == expected, "Incorrect attempt count")
        require(int(summary["Fetch failures"]) == 0 and int(summary["URLs pending"]) == 0, "Incomplete crawl")
        stats = fixture.snapshot()
        identities = {(0, "/")} | {(i, f"/page/{p}") for i, count in enumerate(pages) for p in range(count)}
        require(set(stats["counts"]) == identities and all(n == 1 for n in stats["counts"].values()),
                "Missing, duplicate or unexpected requests")
        global_limit = 1 if engine == "sequential" else concurrency
        require(stats["global_peak"] <= global_limit, "Global cap exceeded")
        require(max(stats["peak_per_origin"]) <= (cap or global_limit), "Origin cap exceeded")
        per_origin_starts = [[t for i, _, t in stats["starts"] if i == h] for h in range(len(pages))]
        # Server arrivals can be delayed/reordered relative to client admission;
        # use broad tolerance here, exact token arithmetic lives in fake-time tests.
        if rate is not None:
            for starts in per_origin_starts:
                require(len(starts) <= burst + rate * (starts[-1] - starts[0] + 0.05) + 1,
                        "Server arrivals exceed the rate envelope tolerance")
        first_start = [next(i for i, (host, _, _) in enumerate(stats["starts"]) if host == h) for h in range(len(pages))]
        return {
            "scenario": "multi-origin", "engine": engine, "configured_global": global_limit,
            "origins": len(pages), "pages_per_origin": ";".join(map(str, pages)), "delay_ms": delay_ms,
            "max_per_host": cap if cap is not None else "disabled", "rate_per_host": rate if rate is not None else "disabled",
            "burst_per_host": burst if rate is not None else "", "requests": expected,
            "process_elapsed_ms": elapsed * 1000, "process_requests_per_second": expected / elapsed,
            "global_peak": stats["global_peak"], "peak_per_origin": ";".join(map(str, stats["peak_per_origin"])),
            "first_start_index_per_origin": ";".join(map(str, first_start)),
            "arrival_span_ms_per_origin": ";".join(f"{(s[-1] - s[0]) * 1000:.3f}" for s in per_origin_starts),
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build-release/fluxcrawl"))
    parser.add_argument("--engine", choices=("sequential", "threaded", "async", "all"), default="all")
    parser.add_argument("--origins", type=positive_int, default=4)
    parser.add_argument("--pages-per-origin", type=positive_int, default=100)
    parser.add_argument("--concurrency", type=positive_int, default=12)
    parser.add_argument("--max-per-host", type=positive_int)
    parser.add_argument("--rate-per-host", type=positive_rate)
    parser.add_argument("--burst-per-host", type=positive_int)
    parser.add_argument("--delay-ms", type=float, default=20)
    parser.add_argument("--runs", type=positive_int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.burst_per_host is not None and args.rate_per_host is None:
        parser.error("--burst-per-host requires --rate-per-host")
    if not math.isfinite(args.delay_ms) or args.delay_ms < 0 or args.warmups < 0:
        parser.error("delay and warmups must be nonnegative")
    if args.origins > 32 or args.concurrency > 256 or args.pages_per_origin > 1000:
        parser.error("local scenario limits: origins <= 32, concurrency <= 256, pages/origin <= 1000")
    if args.output and args.output.exists():
        parser.error("output already exists; choose a fresh path")
    engines = ("sequential", "threaded", "async") if args.engine == "all" else (args.engine,)
    rows = []
    for run in range(-args.warmups, args.runs):
        for engine in engines:
            row = trial(args.binary.resolve(), engine, args.concurrency, [args.pages_per_origin] * args.origins,
                        args.delay_ms, args.max_per_host, args.rate_per_host, args.burst_per_host or 1)
            if run >= 0:
                row["run"] = run + 1
                rows.append(row)
                print(f"{engine} run={run + 1}: requests={row['requests']} global_peak={row['global_peak']} "
                      f"per_origin={row['peak_per_origin']} arrival_span_ms={row['arrival_span_ms_per_origin']}")
    for engine in engines:
        elapsed = [r["process_elapsed_ms"] for r in rows if r["engine"] == engine]
        print(f"{engine}: median process elapsed={statistics.median(elapsed):.3f}ms "
              f"population stddev={statistics.pstdev(elapsed):.3f}ms")
    if args.output:
        # Create only after every sample validates; never overwrite existing evidence.
        with args.output.open("x", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


if __name__ == "__main__":
    main()
