# Step 18 validation and measurements

## Environment and commands

Measured locally on macOS 15.0.1, arm64, Apple Clang 16.0.0, Boost 1.90.0,
Node 22.16.0. All fixtures bind loopback and use temporary state.

* Baseline: existing CMake build succeeded; all **68/68** tests passed after
  allowing loopback binding. The restricted sandbox initially rejected sockets;
  those initial failures were environmental, not crawler assertions.
* Final ordinary build: `cmake -S . -B build`, `cmake --build build -j 4`,
  `ctest --test-dir build --output-on-failure -j 4`: **70/70 passed** (22.58 s).
* Release: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release` and build passed.
* Server disabled: `cmake -S . -B build-no-observe
  -DFLUXCRAWL_OBSERVABILITY=OFF -DBUILD_TESTING=OFF`, build and zero-page CLI smoke
  passed. No Boost server target is required in this configuration.
* ASan + UBSan: Debug with `-fsanitize=address,undefined
  -fno-omit-frame-pointer`; full **70/70** passed. After final checkpoint and
  best-effort telemetry refinements, rebuilt and reran observability/checkpoint
  coverage: **5/5 passed**, no sanitizer findings (15.39 s).
* TSan: Debug with `-fsanitize=thread -fno-omit-frame-pointer`; final threaded
  crawler, observability unit and live integration coverage: **3/3 passed**,
  no race reports (34.86 s).
* Frontend: `npm install`, `npm run typecheck`, `npm run build` passed. npm reported
  zero vulnerabilities at installation. Production output: approximately 234 kB
  JS (73.5 kB gzip), 3.8 kB CSS. Dependencies and dist are ignored; lockfile tracked.
* One pre-existing profiling parity test failed during concurrent compilation and
  test traffic (its fixture uses a 100 ms connect timeout). The full ordinary
  suite passed on the subsequent run without competing compilation. No timing
  assertion was weakened and no existing tests were modified.

CI now installs Boost.JSON on Ubuntu 24.04 and separately checks/builds the
frontend with Node 22. The GitHub-hosted jobs were not executed from this local
session.

Browser setup found no connected browsers (empty browser list). **Visual/browser
QA was not available.** Production index and all built assets were verified over
HTTP, and actual RFC6455 clients verified live data, disconnects and final state.
No screenshots are claimed or added.

## Live integration evidence

`core.observability` verifies immutable publication, concurrent reads, matching
scheduler pending/active/delayed gauges, host counts, no query-string exposure,
engine-specific JSON, and capped latency, errors and topology.

`observability.integration` runs sequential, threaded and async against a
three-origin fanout/retry fixture, with four clients including non-readers. It
asserts active/per-host limits, live sequence updates, exact final seen count,
pressure false → true → false, retry recovery, Bloom target/occupancy/false
positives, SQLite commits/batch sizes/durations, API v1 endpoints, built assets,
invalid CLI/occupied ports, and robots block parity. All clients may disconnect
without affecting normal crawler exit. A separate capped-payload server test
forces stale updates to be dropped and stalled clients disconnected, while a
reading client continues. Its diagnostics verify one in-flight write and shared
snapshot serialization; no unbounded outgoing queue exists.

The manually run demo used `tools/dashboard_demo.py --linger-ms 3000` and an
actual WebSocket client. It finished with:

| Metric | Observed |
| --- | ---: |
| Final state | completed |
| Seen logical URLs | 733 |
| Completed logical resources | 673 |
| Robots blocked | 60 |
| Resource attempts / HTTP responses | 712 / 712 |
| Scheduled / attempted / recovered retries | 39 / 39 / 39 |
| Robots fetches / failures | 3 / 0 |
| Final pending / active / delayed | 0 / 0 / 0 |
| Pressure activations | 15 |
| Time under pressure | 31.202 s |
| Bloom inserted / expected | 733 / 300 |
| Bloom occupancy / false positives | 79.102% / 94 |
| Observed FP among negative membership observations | 12.842% |
| SQLite transactions | 122 |
| Average logical mutations per transaction | 41.730 |
| Total / last commit time | 154.299 / 0.570 ms |
| Final DB bytes, main + WAL | 3,036,448 |
| Persistence errors | 0 |

The deliberate undersized Bloom filter made occupancy and false positives visible;
exact SQLite membership preserved correctness. The observer connected during the
crawl and received 21 snapshots through the completed state. This validates data
flow, not browser appearance.

## Changed file tree

```text
.github/workflows/ci.yml               Boost dependency and separate frontend job
CMakeLists.txt                         core source, optional Boost server target
.gitignore                            frontend build/dependency exclusions
README.md                             dependencies and Live Observability
include/fluxcrawl/
  observability.hpp                   snapshot and bounded aggregator (new)
  observability_server.hpp            server and serialization API (new)
  crawl_types.hpp                     optional observer, last checkpoint metadata
  crawl_policy.hpp                    O(1) held URL/host counters
  host_scheduler.hpp                  bounded host samples
  persistence_control.hpp             last commit duration
src/
  observability.cpp                   aggregation and immutable publication (new)
  observability_sampler.hpp           interruptible joined sampler (new)
  observability_json.cpp              versioned wire model (new)
  observability_server.cpp            HTTP, static assets and WebSockets (new)
  sequential_crawler.cpp              safe sampling around blocking work
  thread_pool_crawler.cpp             capture under coordinator ownership
  async_crawler.cpp                   event-loop sampling and bounded poll waits
  crawl_processing.cpp                bounded completion/discovery observations
  crawl_policy.cpp                    incremental robots gauges
  host_scheduler.cpp                  incremental queue counters, host sample
  sqlite_store.cpp                    last successful commit duration
  persistence_runtime.cpp/.hpp         commit reporting, pending journal gauge
  checkpoint_runtime.cpp              last checkpoint duration/time
  main.cpp                            opt-in server, validation, final grace
dashboard/
  package.json, package-lock.json      React, TypeScript, Vite
  tsconfig.json, vite.config.ts        strict types, local development proxy
  index.html                          dashboard document
  src/schema.ts                       typed schema + defensive envelope parsing
  src/main.tsx                        panels, charts, graph, reconnect/history
  src/style.css                       restrained responsive console
tests/
  CMakeLists.txt                      unit and integration registration
  observability_test.cpp              boundedness and snapshot tests (new)
  observability_integration.py         HTTP/WS/crawler/slow-client tests (new)
tools/
  dashboard_demo.py                   manual deterministic demo (new)
  observability_benchmark.py           repeatable CPU/RSS/timing experiment (new)
docs/
  observability.md                    architecture, schema, limits and usage (new)
  observability-validation.md         this validation report (new)
  observability-performance.md        actual experiment results (new)
```

## Performance result

The [complete overhead report](observability-performance.md) includes all 15 runs,
CPU time and peak RSS. At 500 ms, median elapsed / attempts per second were:

| Configuration | Elapsed s | Attempts/s | CPU s | Peak RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| Dashboard disabled | 2.68429 | 569.61 | 0.25114 | 10.672 |
| Enabled, no clients | 2.64222 | 578.68 | 0.28823 | 11.609 |
| Enabled, one client | 2.67519 | 571.55 | 0.26548 | 11.578 |

100 / 500 / 1000 ms one-client sampling delivered median 26 / 6 / 3 snapshots.
Keep 500 ms for useful live feedback with moderate message volume. Run-to-run
variation prevents a precise universal overhead claim.

## Follow-up with the highest value

Run browser visual QA on a connected desktop browser, then improve host selection
with a bounded rotating or incrementally ranked candidate set. The current fixed
candidate sample is cheap and explicitly labeled, but an incremental rank would
make a very large multi-origin crawl easier to diagnose without full-map scans.
This does not require distributed crawling or a control plane.
