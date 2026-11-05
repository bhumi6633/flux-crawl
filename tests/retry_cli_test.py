"""Retry CLI validation and actual local recovery/exit-status checks."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from multi_origin import MultiOrigin

binary = sys.argv[1]
seed = "http://127.0.0.1:1/"
for flag, values in {
    "--max-retries": ["-1", "x", "1.5", "184467440737095516160"],
    "--retry-backoff-ms": ["0", "-1", "x", "1.5", "9223372036854775808", "1e9"],
    "--retry-max-backoff-ms": ["0", "-1", "x", "1.5", "9223372036854775808"],
}.items():
    for value in values:
        result = subprocess.run([binary, "--max-pages", "0", flag, value, seed], capture_output=True, text=True)
        assert result.returncode != 0, (flag, value, result.stdout)
    result = subprocess.run([binary, flag], capture_output=True, text=True)
    assert result.returncode != 0
for flags in (["--retry-backoff-ms", "100", "--retry-max-backoff-ms", "99"],
              ["--retry-max-backoff-ms", "1"], ["--retry-multiplier", "nan"]):
    result = subprocess.run([binary, "--max-pages", "0", *flags, seed], capture_output=True, text=True)
    assert result.returncode != 0
for engine in ("sequential", "threaded", "async"):
    result = subprocess.run([binary, "--engine", engine, "--max-pages", "0", "--max-retries", "0", seed],
                            capture_output=True, text=True)
    assert result.returncode == 0 and "Retries scheduled:" not in result.stdout
    for path, expected_attempts, expected_recovered, expected_exit in (
            ("/flaky-503", 3, 1, 0), ("/close", 2, 1, 0), ("/always-503", 3, 0, 1)):
        with MultiOrigin((1, 1), [0, 0], graph="retry") as fixture:
            result = subprocess.run([binary, "--engine", engine, "--max-retries", "2", "--retry-backoff-ms", "10",
                                     "--retry-max-backoff-ms", "40", "--retry-jitter", "--no-retry-jitter",
                                     "--max-pages", "10", fixture.url(path=path)], capture_output=True, text=True, timeout=30)
            assert result.returncode == expected_exit, result.stdout + result.stderr
            assert f"Pages attempted: {expected_attempts}" in result.stdout, result.stdout
            assert f"Recovered after retry: {expected_recovered}" in result.stdout
            assert f"Retries attempted: {expected_attempts - 1}" in result.stdout
            assert "URLs discovered: 1" in result.stdout and "URLs pending: 0" in result.stdout
            print(engine, path, result.stdout.strip().replace("\n", "; "))
for flag in ("--max-retries", "--retry-backoff-ms", "--retry-max-backoff-ms", "--retry-jitter", "--no-retry-jitter"):
    result = subprocess.run([binary, "benchmark", flag, "2", "--seed", seed], capture_output=True, text=True)
    assert result.returncode != 0
print("Retry CLI validation, defaults, flag ordering, recovery and terminal failure status passed")
