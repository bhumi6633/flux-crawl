# Reproducible local demo and recovery

All commands run from the repository root unless a `cd` is shown. They use
existing fixture tooling; no new subsystem or external service is required.
Keep the host awake, serialize network experiments, and stop fixtures with Ctrl-C.
The full systems demo owns its fixture lifecycle and temporary state automatically.

## Full systems demo

Prerequisites match [Build](../README.md#build): CMake/C++ dependencies including
Boost for the server, Python 3, and Node 22.16+ for static assets.

```sh
cd dashboard
npm ci
npm run test
npm run typecheck
npm run build
cd ..
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
python3 tools/dashboard_demo.py
```

The script starts three loopback origins and FluxCrawl async with eight slots,
per-origin cap two, robots enabled, two retries, pressure 60/15, Bloom+SQLite and
batch threshold 24. It prints a dashboard address on port 8080; open that URL
while the crawl runs. The server serves built assets from this source checkout.
If port 8080 is occupied, this equivalent invocation selects an available port:

```sh
python3 tools/dashboard_demo.py --port 0 --linger-ms 15000
```

The fixture has 733 logical resources: 673 completed, 60 robots-blocked, with
39 recovered retries and 712 resource attempts in the certified run. It presents
varied latency and enough discovery to trigger pressure; expect roughly 30–90
seconds including linger. Timing and sampled snapshot counts can vary. The Bloom design capacity is only
300 for 733 identities, so the capacity-exceeded indicator is expected; SQLite
still makes exact membership decisions. The
fixture and database are temporary and removed after the script exits.

Walk through active transfers, host caps, retries, pressure/deferred work,
robots blocks, exact/Bloom checks and storage metrics. Use the
[interview talking points](interview-guide.md#demo-talking-points). A port-zero
run is functionally identical; do not promise a fixed URL when using it.

No verified dashboard screenshot is included. Automated transport/schema tests
and a live fixture run establish data behavior, not visual browser layout.

## Small local crawl

Terminal A:

```sh
python3 tools/benchmark_server.py --port 8081 --scenario latency-bound
```

Terminal B:

```sh
./build/fluxcrawl --engine async --concurrency 4 --depth 20 --max-pages 100 http://127.0.0.1:8081/page/0
```

The graph has 100 pages. The depth 20 limit includes the graph; default depth two
would intentionally fetch a smaller subset. Use this fixture for the recovery
and benchmark examples below; keep terminal A running through resume.

## Recovery examples

Use a new temporary directory so new-state commands cannot conflict with old
files. Companions belong with their checkpoint/database; do not move just the
primary during an interrupted crawl.

### Snapshot: sequential stop, async resume

```sh
flux_state_dir=$(mktemp -d /tmp/fluxcrawl-snapshot.XXXXXX)
./build/fluxcrawl --engine sequential --depth 20 --max-pages 150 \
  --checkpoint "$flux_state_dir/crawl.chk" --checkpoint-every 1 \
  --stop-after-attempts 5 http://127.0.0.1:8081/page/0
./build/fluxcrawl resume "$flux_state_dir/crawl.chk" --engine async --concurrency 4
```

The clean stop exits successfully with unfinished logical work. Resume retains
stored semantic configuration/budget and selects a different execution engine.
Its cumulative result should finish the 100-resource graph. This is a graceful
stop example; crash/active reissue semantics are tested separately.

### SQLite: threaded stop, async resume with exact Bloom-backed membership

```sh
flux_db_dir=$(mktemp -d /tmp/fluxcrawl-sqlite.XXXXXX)
./build/fluxcrawl --engine threaded --workers 4 --depth 20 --max-pages 150 \
  --state-db "$flux_db_dir/crawl.db" --dedup bloom-sqlite --bloom-expected 100 \
  --db-batch-size 24 --stop-after-attempts 5 http://127.0.0.1:8081/page/0
./build/fluxcrawl resume "$flux_db_dir/crawl.db" --engine async --concurrency 4
```

The new database must not exist. Bloom never replaces exact membership. Resume
can reissue work from its durable boundary; do not describe either example as
exactly-once HTTP. Retain these temporary directories for inspection, or remove
only the paths you created after finishing the demo.

## Short benchmark

With the same server still running and the Release binary built:

```sh
./build-release/fluxcrawl benchmark --seed http://127.0.0.1:8081/page/0 \
  --scenario latency-bound --engines all --concurrency 1,4,16 \
  --runs 3 --warmups 1 --output /tmp/fluxcrawl-latency.csv
```

CSV output is exclusively created, so choose a new name if that path exists.
Do not run this alongside tests, other benchmarks or the systems demo. This is
one profile, not a reproduction of all final report data. Full profile and feature
protocols are in the [benchmark report](benchmark-report.md).

## Verification record

Step 20 reruns the documented configure/build/test sequence, frontend
install/test/typecheck/build, local crawl, benchmark, both cross-engine resume
examples and full demo. Exact results and link checks are recorded in
[packaging verification](packaging-verification.md). Public-site examples and
platform-specific package-install commands are reviewed recipes, not executed
external crawls or claims of installation on every OS.
