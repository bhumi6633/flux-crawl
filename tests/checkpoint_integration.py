"""Fresh-process recovery, engine switching, active reissues, deadlines and policy reset."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from checkpoint_fixture import CheckpointFixture

DRIVER = sys.argv[1]
FIELDS = "attempted discovered pending completed blocked unresolved recovery retried robots checkpoints".split()


def run(site, directory, engine, stop="none", budget=100, robots=False, resume=None, capture="none", name="state"):
    output = directory / (name + ".chk")
    result = subprocess.run([DRIVER, "resume" if resume else "start", str(resume) if resume else site.url(), engine,
                             str(output), str(stop), str(budget), str(int(robots)), "8", capture],
                            text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [line.split("\t") for line in result.stdout.splitlines()]
    return output, rows[:-1], dict(zip(FIELDS, map(int, rows[-1][1:]), strict=True))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        with CheckpointFixture() as reference_site:
            _, _, reference_result = run(reference_site, directory, "sequential", name="reference")
            assert reference_result["completed"] == 11
            reference = set(reference_site.snapshot()["counts"])
        for initial, resumed in (("sequential", "async"), ("threaded", "async"), ("async", "threaded")):
            with CheckpointFixture() as site:
                saved, _, partial = run(site, directory, initial, stop=3, name=initial)
                assert partial["attempted"] == 3 and partial["pending"] > 0, partial
                root_before = site.snapshot()["counts"][(0, "/")]
                _, records, final = run(site, directory, resumed, resume=saved, name=initial + "-resumed")
                assert final["attempted"] == final["discovered"] == final["completed"] == 11, final
                assert final["pending"] == final["unresolved"] == final["recovery"] == 0
                assert site.snapshot()["counts"][(0, "/")] == root_before == 1
                assert all(n == 1 for n in site.snapshot()["counts"].values()), site.snapshot()
                assert set(site.snapshot()["counts"]) == reference
                assert all(row[2] == "3" for row in records if row[0] == "record" and row[1].endswith("/leaf"))
                print(initial, "->", resumed, "graceful resume", final)
        # Snapshot active leases, discard newer completion knowledge, and resume a different engine.
        for initial, resumed in (("threaded", "async"), ("async", "threaded")):
            with CheckpointFixture() as site:
                saved, _, _ = run(site, directory, initial, stop=3, capture="active", name=initial + "-active")
                active = Path(str(saved) + ".active")
                assert active.exists()
                text = active.read_text()
                assert "FLUXCRAWL_CHECKPOINT 2" in text and "work " in text
                _, _, final = run(site, directory, resumed, resume=active, name=initial + "-recovered")
                assert final["pending"] == 0 and final["discovered"] == 11 and final["completed"] == 11, final
                assert final["recovery"] > 0 and final["unresolved"] > 0
                assert site.snapshot()["counts"][(0, "/")] == 1  # Completed root never reissued.
                assert max(site.snapshot()["peak_per_origin"]) <= 2
                assert set(site.snapshot()["counts"]) == reference
                print(initial, "active ->", resumed, final)
        for resumed in ("sequential", "threaded", "async"):
            with CheckpointFixture() as site:
                saved, _, partial = run(site, directory, "sequential", stop=8, budget=10, name="budget-" + resumed)
                assert partial["attempted"] == 8
                _, _, final = run(site, directory, resumed, resume=saved, name="budget-final-" + resumed)
                assert final["attempted"] == 10 and final["pending"] == 1, final
        with CheckpointFixture("retry") as site:
            saved, _, partial = run(site, directory, "sequential", stop=4, name="retry")
            assert partial["retried"] == 1 and partial["attempted"] == 4
            text = saved.read_text()
            assert '"' + site.url(path="/flaky") + '" 1 2 0 0 1 ' in text, text
            _, records, final = run(site, directory, "async", resume=saved, name="retry-resumed")
            assert final["retried"] == 2 and final["attempted"] == 7 and final["discovered"] == 5, final
            flaky = [t for h, p, t in site.snapshot()["starts"] if h == 0 and p == "/flaky"]
            assert flaky[2] - flaky[1] >= 0.3
            assert any(row[0] == "record" and row[1].endswith("/tail") and row[2] == "2" for row in records)
        with CheckpointFixture() as site:
            saved, _, partial = run(site, directory, "sequential", stop=3, robots=True, name="robots")
            assert partial["blocked"] == 1
            before = site.snapshot()["counts"]
            _, _, final = run(site, directory, "async", resume=saved, name="robots-resumed")
            after = site.snapshot()["counts"]
            assert final["completed"] == 10 and final["blocked"] == 1 and final["pending"] == 0
            assert (0, "/private/blocked") not in after and after[(0, "/")] == 1
            assert {key for key in after if key[1] != "/robots.txt"} == reference - {(0, "/private/blocked")}
            assert sum(n for (h, p), n in after.items() if p == "/robots.txt") > sum(n for (h, p), n in before.items() if p == "/robots.txt")
        with CheckpointFixture() as site:
            _, _, paused = run(site, directory, "async", capture="pause", name="pause")
            assert paused["attempted"] == paused["completed"] == 1
        print("Checkpoint recovery, dedup, depth, retry deadlines, budgets and robots reacquisition passed")


if __name__ == "__main__":
    main()
