# Resume, GitHub and portfolio material

## Recommended project entry

**FluxCrawl — Network Concurrency & Performance Engineering**  
C++20, libcurl, libxml2, SQLite, Boost.Beast/Asio, React/TypeScript, CMake

### Three primary bullets

* Built three C++20 crawl engines—sequential, fixed thread pool and libcurl multi—
  over shared semantics to compare blocking and event-driven I/O.
* Implemented origin-aware scheduling and lossless bounded admission, preserving
  retry capacity while deferring excess discovery to disk.
* Measured 218 versus 18 attempts/s for async/16 versus sequential on a controlled
  50 ms local workload; documented where parallel parsing instead favored threads.

### Two alternate bullets

* Implemented snapshot/SQLite recovery across execution engines; injected storage
  write failures to verify committed-prefix recovery and at-least-once replay.
* Built Bloom-assisted exact SQLite deduplication; validated a 100,101-URL restart
  workload without allowing probabilistic membership to discard URLs.

Use the primary set for architecture/performance roles. Substitute an alternate
when storage/reliability is more relevant. Avoid putting all five in one entry.
The throughput number is rounded from 218.3/18.1 attempts/s, not a claim of a
universal speedup or a before/after optimization of the same architecture.

## Claim evidence map

| Claim | Code | Test / measurement | Tradeoff and failure defense |
| --- | --- | --- | --- |
| Three engines with shared semantics | [engine loops](design-walkthrough.md#4-reserve-and-execute), shared crawl processing/state/policy | Engine equivalence and benchmark integration suites; [84-sample report](benchmark-report.md) | Ownership differs; async parsing is serialized, workers coordinate |
| Origin-aware bounded admission | [scheduler](../src/host_scheduler.cpp), [state](../src/crawl_state.cpp), [spool](../src/discovery_spool.cpp) | Scheduling/pressure parity and 10k stress; [bound contract](backpressure.md) | Disk/records can grow; tiny bounds limit concurrency; redirect hops share original lease |
| 218 vs 18 attempts/s | [benchmark runner](../src/benchmark.cpp) | [latency-bound table](benchmark-report.md#latency-bound), selected CSV/environment | Apple M3, Release, limit 16, three runs + warmup; fixture-specific |
| Cross-engine durable-prefix recovery | [SQLite](../src/sqlite_store.cpp), [persistence runtime](../src/persistence_runtime.cpp) | [fault/recovery certification](certification.md#faults-chaos-and-recovery), six engine/write-error cases | A durable prefix can replay active/uncommitted attempts; no exactly-once HTTP |
| Bloom-assisted exactness and 100,101 restart | [seen backends](../src/seen_set.cpp), [Bloom](../src/bloom_filter.cpp) | [certification](certification.md#final-execution-results), dedup parity/restart tests | Saturation increases exact checks; SQLite remains authoritative |

For ownership and failure explanations, use the [walkthrough](design-walkthrough.md)
and [interview guide](interview-guide.md). These claims do not require remote CI
or a release tag to have run; do not describe those unexecuted gates as passed.

## GitHub repository description

C++20 network-concurrency benchmark with sequential, thread-pool and libcurl multi engines, bounded scheduling, durable recovery and live observability.

Suggested topics (copy into repository settings manually):

```text
cpp cpp20 systems-programming networking concurrency libcurl web-crawler
performance benchmarking sqlite observability
```

This file prepares metadata; it does not update GitHub settings or publish anything.
The current project name is FluxCrawl, version 0.1.0 candidate, MIT licensed.

## Portfolio description — 114 words

FluxCrawl explores how networking architecture changes system behavior when the
application workload stays consistent. I implemented sequential blocking, fixed
thread-pool and single-threaded libcurl multi engines in C++20, sharing URL
identity, parsing and scheduling rules. Deterministic local workloads expose when
overlapping requests helps and when serialized processing limits an event loop.
The project also addresses work admission, origin fairness, retries, exact
disk-backed deduplication and crash recovery. A React console makes queue
pressure, latency and persistence visible through bounded telemetry. Controlled
benchmarks, parser fuzzing, storage fault injection and lifecycle testing provide
evidence for the design decisions. The result is an inspectable systems
experiment with documented tradeoffs, rather than a claim that one concurrency
model always wins.

## Short application description — 58 words

FluxCrawl is a C++20 systems project comparing sequential, thread-pool and
libcurl multi execution over shared crawl semantics. I built deterministic
benchmarks, origin-aware scheduling, bounded admission, exact Bloom-assisted
SQLite membership and cross-engine recovery. A local React dashboard exposes
runtime behavior. The project combines performance measurements with parser
fuzzing and injected HTTP/storage failures to explain architectural tradeoffs
under controlled workloads.

## Avoid these substitutions

Keep “attempts/s” distinct from successful responses/s, accepted URLs and public
internet capacity. Keep “single crawl event loop” distinct from a process with no
other threads. Keep “exact deduplication” distinct from exactly-once HTTP delivery.
Do not claim production deployment, distributed operation, universal constant
memory, or remote CI certification. The [release gates](certification.md#ci-and-remaining-release-gates)
remain explicit.
