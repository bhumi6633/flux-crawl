"""Politeness CLI validation without network access."""
import subprocess
import sys

binary = sys.argv[1]
seed = "http://127.0.0.1:1/"
for flag, values in {
    "--max-per-host": ["0", "-1", "x", "2.5", "184467440737095516160"],
    "--rate-per-host": ["0", "-1", "nan", "NaN", "inf", "Infinity", "1e999", "1e-999", "1junk", " 2", "2 "],
    "--burst-per-host": ["0", "-1", "x", "1.5"],
}.items():
    for value in values:
        result = subprocess.run([binary, "--max-pages", "0", flag, value, seed], capture_output=True, text=True)
        assert result.returncode != 0, (flag, value, result.stdout)
    result = subprocess.run([binary, flag], capture_output=True, text=True)
    assert result.returncode != 0
result = subprocess.run([binary, "--burst-per-host", "2", "--max-pages", "0", seed], capture_output=True, text=True)
assert result.returncode != 0 and "requires --rate-per-host" in result.stderr
for engine in ("sequential", "threaded", "async"):
    for flags in (["--rate-per-host", "2.5", "--burst-per-host", "3"],
                  ["--burst-per-host", "3", "--rate-per-host", "2.5"]):
        result = subprocess.run([binary, "--engine", engine, "--max-pages", "0", "--max-per-host", "2", *flags, seed],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert "Max per host: 2" in result.stdout and "2.5 requests/s; burst 3" in result.stdout
        assert "Pages attempted: 0" in result.stdout
result = subprocess.run([binary, "--max-pages", "0", seed], capture_output=True, text=True)
assert "Max per host: unlimited" in result.stdout and "Rate per host: disabled" in result.stdout
for flag in ("--max-per-host", "--rate-per-host", "--burst-per-host"):
    result = subprocess.run([binary, "benchmark", flag, "2", "--seed", seed], capture_output=True, text=True)
    assert result.returncode != 0
print("Politeness numeric validation, defaults, option ordering, engines and benchmark rejection passed")
