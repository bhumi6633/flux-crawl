"""CLI graceful/crash resume, immutable semantics, final markers and failure reporting."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from checkpoint_fixture import CheckpointFixture

binary = sys.argv[1]


def command(*args):
    return subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=60)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        file = directory / "crawl.chk"
        for flag in ("--checkpoint-every", "--stop-after-attempts"):
            for invalid in ("-1", "x", "1.5", "184467440737095516160"):
                assert command(flag, invalid, "http://127.0.0.1:1/").returncode != 0
        assert command("--checkpoint-every", "0", "http://127.0.0.1:1/").returncode != 0
        assert command("--checkpoint-every", "2", "http://127.0.0.1:1/").returncode != 0
        assert command("--checkpoint").returncode != 0
        assert command("resume", directory / "absent.chk").returncode != 0
        with CheckpointFixture() as site:
            partial = command("--engine", "threaded", "--workers", "8", "--checkpoint", file,
                              "--checkpoint-every", "2", "--stop-after-attempts", "3", "--depth", "3", "--max-pages", "100", site.url())
            assert partial.returncode == 0, partial.stdout + partial.stderr
            assert "Pages attempted: 3" in partial.stdout and "Checkpoint stop reached" in partial.stdout
            text = file.read_text()
            assert text.startswith("FLUXCRAWL_CHECKPOINT 3\n") and "work " in text and "0x" not in text
            for flags in (("--depth", "2"), ("--max-pages", "200"), ("--respect-robots", "1"), ("--engine", "bad"), ("--workers", "0")):
                assert command("resume", file, *flags).returncode != 0
            resumed = command("resume", file, "--engine", "async", "--concurrency", "16")
            assert resumed.returncode == 0 and "Pages attempted: 11" in resumed.stdout and "URLs pending: 0" in resumed.stdout, resumed.stdout + resumed.stderr
            counts = site.snapshot()["counts"]
            assert all(n == 1 for n in counts.values()), counts
            done = command("resume", file)
            assert done.returncode == 0 and "already completed" in done.stdout
            assert counts == site.snapshot()["counts"]
            corrupted = directory / "corrupt.chk"
            corrupted.write_text(text[:40])
            assert command("resume", corrupted).returncode != 0
            assert counts == site.snapshot()["counts"]
        # Abrupt process termination after a periodic snapshot with active work.
        with CheckpointFixture() as site:
            site.delays = (0.12,) * 3
            crash = directory / "crash.chk"
            process = subprocess.Popen([binary, "--engine", "threaded", "--workers", "8", "--checkpoint", str(crash),
                                        "--checkpoint-every", "2", "--depth", "3", "--max-pages", "100", site.url()],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 30
                while not crash.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.005)
                assert crash.exists(), process.communicate(timeout=5)
                snapshot = crash.read_text()
                assert snapshot.endswith("END\n") and "unfinished " in snapshot
                process.terminate()
                process.communicate(timeout=30)
                assert process.returncode != 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=30)
            resumed = command("resume", crash, "--engine", "async", "--concurrency", "16")
            assert resumed.returncode == 0 and "URLs pending: 0" in resumed.stdout, resumed.stdout + resumed.stderr
            assert "Recovery attempts: 0" not in resumed.stdout
            assert site.snapshot()["counts"][(0, "/")] == 1
            print("Abrupt threaded -> async recovery:\n" + resumed.stdout)
        # Optional checkpoint failure is reported while resource crawling continues.
        with CheckpointFixture() as site:
            failed = command("--checkpoint", directory / "absent-parent" / "bad.chk", "--checkpoint-every", "1", "--max-pages", "1", site.url())
            assert failed.returncode != 0 and "Checkpoint failed:" in failed.stderr
            assert "Pages attempted: 1" in failed.stdout and "Checkpoints written: 0" in failed.stdout
        for flag in ("--checkpoint", "--checkpoint-every"):
            assert command("benchmark", flag, file).returncode != 0
        print("Checkpoint CLI corruption, configuration, graceful/crash recovery and write-failure checks passed")


if __name__ == "__main__":
    main()
