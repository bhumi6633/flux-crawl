"""Library and CLI contract tests; no performance thresholds."""
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from benchmark_server import BenchmarkServer, Workload, SCENARIOS, scenario_workload


def main():
    environment = dict(os.environ)
    # The benchmark must bypass proxies itself, including a hostile ambient setting.
    environment.update(http_proxy="http://127.0.0.1:1", HTTP_PROXY="http://127.0.0.1:1", NO_PROXY="", no_proxy="")
    with BenchmarkServer(0, Workload(10, 4, 2, 1024)) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            seed = f"http://127.0.0.1:{server.server_port}/page/0"
            subprocess.run([sys.argv[1], seed], env=environment, check=True, timeout=40)
            cli = [sys.argv[2], "benchmark", "--seed", seed, "--concurrency", "1,2,4", "--runs", "2", "--warmups", "1"]
            with tempfile.TemporaryDirectory(prefix="fluxcrawl-benchmark-") as temporary:
                output = Path(temporary) / "results.csv"
                before = server.resets
                result = subprocess.run(cli + ["--output", str(output)], capture_output=True, text=True, env=environment, timeout=40)
                assert result.returncode == 0, result.stderr
                rows = list(csv.DictReader(output.open()))
                assert len(rows) == 14
                assert all(row["profile_enabled"] == "0" and row["extraction_ms"] == ""
                           and row["worker_profiles"] == "" and None not in row for row in rows)
                assert all(row["measured_runs"] == "2" and int(row["started_at_unix_ms"]) > 0 for row in rows)
                assert server.resets - before == 21  # 7 configurations x (one warmup + two measured).
                assert all(int(row["pages_attempted"]) == 10 and int(row["bytes_downloaded"]) == 10240 for row in rows)
                assert all(int(row["latency_samples"]) == 10 and float(row["elapsed_ms"]) >= 0 for row in rows)
                assert all(int(row["server_peak_active"]) <= int(row["configured_concurrency"]) for row in rows)
                assert all(row["scenario"] == "custom" and float(row["latency_min_ms"]) <= float(row["latency_mean_ms"]) <= float(row["latency_max_ms"]) for row in rows)
                assert "Medians across measured runs" in result.stdout
                assert "elapsed_stddev_ms=" in result.stdout
                separate = Path(temporary) / "separate.csv"
                result = subprocess.run(cli + ["--engine", "all", "--workers", "1,2", "--runs", "1", "--warmups", "0",
                                               "--scenario", "custom", "--output", str(separate)],
                                        capture_output=True, text=True, env=environment, timeout=20)
                assert result.returncode == 0, result.stderr
                with separate.open() as source:
                    split_rows = list(csv.DictReader(source))
                assert [(row["engine"], row["configured_concurrency"]) for row in split_rows] == [
                    ("sequential", "1"), ("threaded", "1"), ("threaded", "2"),
                    ("async", "1"), ("async", "2"), ("async", "4")]
                # Exercise all preset topologies with tiny overrides; never run the full matrix in CI.
                for name in SCENARIOS:
                    server.workload = scenario_workload(name, pages=10, delay_ms=0)
                    destination = Path(temporary) / (name + ".csv")
                    result = subprocess.run([sys.argv[2], "benchmark", "--seed", seed, "--scenario", name,
                                             "--engine", "all", "--concurrency", "2", "--runs", "1", "--profile",
                                             "--output", str(destination)], capture_output=True, text=True, env=environment, timeout=20)
                    assert result.returncode == 0, result.stderr
                    with destination.open() as source:
                        scenario_rows = list(csv.DictReader(source))
                    assert "Profile (overlapping wall durations" in result.stdout
                    for row in scenario_rows:
                        assert None not in row and row["profile_enabled"] == "1"
                        assert int(row["extractions"]) == 10 and int(row["links_accepted"]) == 9
                        assert float(row["extraction_ms"]) >= 0
                        assert row["delay_pattern"] == server.workload.delay_pattern
                        assert int(row["shared_anchors"]) == server.workload.shared_anchors
                        if row["engine"] == "threaded":
                            assert int(row["worker_requests"]) == 10 and row["multi_wait_ms"] == ""
                        elif row["engine"] == "async":
                            assert int(row["completions"]) == 10 and row["lock_wait_ms"] == ""
                        else:
                            assert row["lock_wait_ms"] == row["multi_wait_ms"] == ""
                    assert len(scenario_rows) == 3 and all(row["scenario"] == name for row in scenario_rows)
                    assert all(int(row["bytes_downloaded"]) == 10 * server.workload.body_bytes for row in scenario_rows)
                server.workload = Workload(10, 4, 2, 1024)
                original = output.read_bytes()
                failed = subprocess.run(cli + ["--output", str(output)], capture_output=True, text=True, env=environment, timeout=10)
                assert failed.returncode == 1 and output.read_bytes() == original
                invalid_cases = (["--runs", "0"], ["--engines", ""], ["--engines", "fake"], ["--concurrency", "0"],
                                 ["--concurrency", "1,"], ["--runs", "-1"], ["--runs", "999999999999999999999999"],
                                 ["--output", str(Path(temporary) / "missing" / "file.csv")], ["--warmups"], ["--workers", "0"], ["--workers", "-1"], ["--workers", "1,,2"],
                                 ["--workers", "999999999999999999999999999"], ["--workers", "10000"],
                                 ["--concurrency", "2048"], ["--engine", "async", "--workers", "2"],
                                 ["--scenario", "latency-bound"], ["--runs", "10001"])
                for arguments in invalid_cases:
                    failed = subprocess.run(cli + list(arguments), capture_output=True, text=True, env=environment, timeout=10)
                    assert failed.returncode == 1, arguments
                # Valid counts alone are insufficient: a missing link must invalidate a trial.
                original_children = server.workload.children
                server.workload.children = lambda page: [child for child in original_children(page) if child != 9]
                invalid_output = Path(temporary) / "invalid.csv"
                failed = subprocess.run(cli + ["--output", str(invalid_output)], capture_output=True, text=True, env=environment, timeout=10)
                assert failed.returncode == 1 and "Invalid benchmark sample" in failed.stderr, failed.stderr
                assert not invalid_output.exists() and not failed.stdout
            print("Benchmark library, sweeps, warmups, CSV, CLI validation, and incomplete-workload checks passed")
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
