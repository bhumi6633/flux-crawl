"""Robots CLI parity, summary, disabled mode and terminal-policy status."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from robots_fixture import RobotsFixture

binary = sys.argv[1]
for engine in ("sequential", "threaded", "async"):
    for mode, flags, blocked, expected_exit in (
            ("prefix", ["--respect-robots"], 1, 0),
            ("root", ["--respect-robots"], 1, 0),
            ("forbid", ["--respect-robots"], 1, 1),
            ("prefix", ["--respect-robots", "--ignore-robots"], 0, 0)):
        with RobotsFixture((mode,)) as site:
            result = subprocess.run([binary, "--engine", engine, *flags, "--depth", "2", "--max-pages", "100", site.url()],
                                    text=True, capture_output=True, timeout=30)
            assert result.returncode == expected_exit, result.stdout + result.stderr
            counts = site.snapshot()["counts"]
            if flags[-1] == "--respect-robots":
                assert f"URLs blocked by robots: {blocked}" in result.stdout, result.stdout
                assert "Robots fetches: 1" in result.stdout
                assert counts[(0, "/robots.txt")] == 1
                assert (0, "/private/secret") not in counts
            else:
                # /robots.txt is explicitly linked, so this is a normal resource fetch.
                assert "Robots fetches:" not in result.stdout
                assert counts[(0, "/private/secret")] == 1
            assert all(agent.startswith("FluxCrawl/") for agent in site.snapshot()["user_agents"])
for flag in ("--respect-robots", "--ignore-robots"):
    result = subprocess.run([binary, "benchmark", flag], text=True, capture_output=True)
    assert result.returncode != 0
print("Robots CLI summaries, explicit disabling, engine parity and fallback exit status passed")
