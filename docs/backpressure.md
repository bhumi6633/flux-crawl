# Step 16: bounded pending work

## Baseline before admission changes

Release, deterministic graph: seed →20 branch pages →500 unique leaves each,
three loopback origins, depth 2. Sequential process stopped at increasing attempt
budgets, before fetching leaves. Pending grows monotonically during this phase,
so the final 10,000 pending items are also its peak. External `/usr/bin/time -l`
measured process peak RSS (Darwin bytes). Separate runs, warm local fixture:

| Attempts | Pending | Seen | Peak RSS bytes |
| ---: | ---: | ---: | ---: |
| 1 | 20 | 21 | 6,864,896 |
| 6 | 2,515 | 2,521 | 9,011,200 |
| 11 | 5,010 | 5,021 | 10,387,456 |
| 16 | 7,505 | 7,521 | 12,288,000 |
| 21 | 10,000 | 10,021 | 13,107,200 |

The full pre-change suite passed 64/64 before implementation (55.14 s).
`tools/backpressure_fixture.py` defines the graph. Raw CSV is ignored in results/.

## Bound and admission contract

The default is enabled: high=10,000 and low=7,500. `--unbounded-pending`
explicitly disables it. `--pending-high-watermark N` alone derives floor(3N/4)
as low; an explicit low must satisfy 0<=low<high, with high>0. Negative, malformed,
overflowing and conflicting values fail. The library exposes BackpressureOptions.

Queued ordinary pending means ready resources + delayed resource retries +
robots-held resources. It excludes active requests, infrastructure bootstrap work,
terminal resources and unadmitted candidates on disk. The hard guarantee is
queued ordinary pending <= high, with **zero overshoot**. To preserve retry slots,
admission enforces the stronger accepted-unfinished <= high, including active
ordinary requests as reservations. Active requests are not reported as queued.
Consequently a tiny high can constrain useful network concurrency below the engine's
configured limit. Acquiring a resource moves its reservation from queued to active;
retry moves it back without acquiring new capacity. Terminal completion releases it.
Robots infrastructure is exempt so policy resolution cannot deadlock behind held
resources; held ordinary URLs do count. Global limits apply across all origins.

The gate enters pressure when reservations reach high. It stays active until
reservations fall to or below low, even if some consumers removed queued work.
Only these transitions increment activation/resumption counters. Pressure time is
steady-clock wall time in that state, not summed worker waiting time.

New links are normalized by Url and offered without marking them seen. They enter
an append-only disk spool, partitioned by depth and origin. Admission checks exact
seen state and available reservation capacity under the existing coordinator mutex,
then inserts seen and transfers the item directly to scheduler or robots policy.
If saturated, the candidate remains on disk and remains unseen. Cross-page deferred
duplicates can exist on disk; they collapse at exact admission. Deferred counts are
candidate records, not necessarily distinct URLs. URLs discovered retains its old
meaning: accepted canonical identities, including seed.

Before pressure, a monotonic record sequence preserves FIFO publication order.
After pressure activates, admission rotates among nonempty origins at the shallowest
available depth. HostScheduler retains its host fairness, rate limits and depth
barriers. No per-host pending quota was added. Depth is recorded unchanged; only
starting a normal request consumes max_pages. At an early page-budget stop, bounded
mode can report fewer accepted/seen URLs and more deferred candidates than unbounded
mode. Those candidates remain recoverable when persistence is enabled. A complete
crawl with sufficient attempt/depth budgets has the same logical reachable URL set.

## Producer flow control and memory

Bounded mode uses libxml2 DOM traversal with a callback. It avoids XPath's full
match array and the extractor's document-sized URL vector/reference/hash sets.
Each completed response streams unique normalized URLs into a disposable SQLite
temporary table with a 256 KiB page-cache target and a bound unique insert. It
preserves per-document deduplication and first occurrence order. This temporary
index is not the global seen index, and its journal/synchronous settings are
nondurable because the active parent remains recoverable until publication.
The original vector extractor remains available and is used in unbounded mode.

The temporary table streams into the shared deferred spool one URL at a time;
no completed-page URL vector waits in RAM. The spool caches at most 16 open file
descriptors. Its in-memory index is proportional to origin/depth partitions, not
URL count within those partitions. File storage and consumed prefixes grow until
the spool is retired; this phase does not impose a disk quota or compact the log.
Each scheduling pass admits at most 64 new URLs. Duplicate scanning also yields
periodically, limiting coordinator monopolization and persistence mutation bursts.

Threaded producers do not wait for capacity: they finish disk publication, release
the coordinator and become consumers again. This intentionally avoids the case
where all workers wait as producers and none can drain the queue. Existing consumer
condition-variable waits release the coordinator mutex and recheck scheduling,
termination and failure in a loop, including after spurious wakeups. There is no
producer sleep polling, producer condition-variable wait, or producer wait-time
metric to conflate with pressure wall time.

Async never blocks waiting for admission capacity. A completion publishes its
bounded-memory disk representation, then refill continues consuming admitted work;
subsequent passes resume admission. Already-active transfers continue through curl.
Non-HTML responses do not create a discovery spool batch. Synchronous local spool
I/O, normalization and DB commits can still pause the event loop; this design bounds
queues rather than claiming disk I/O is asynchronous. Disk/DB latency remains a
future optimization opportunity, measured separately below.

This is **not a strict bound on all process memory**. Exact seen strings, attempt
records, host/policy tables, per-partition spool metadata, response bodies, libxml2
DOMs, active transfers and SQLite caches still consume memory. Active response and
temporary-table count is limited by engine concurrency; bodies have the existing
HTTP size limit. Arbitrarily many distinct origins/depths can grow metadata, and
seen/record memory continues to grow across a complete crawl. The bounded resource
is accepted unfinished queue work, not all discovered information or total RSS.

## Persistence and recovery

Snapshot/metadata payload format 2 records watermarks plus a deferred companion
manifest: absolute directory, per-partition file ID/origin/depth, consumed and
published byte offsets, remaining record count and record-order sequence. Format 1
snapshots and SQLite payloads still load with admission disabled. SQLite schema
remains version 1 because its tables are unchanged; its versioned metadata payload
carries the manifest. Logical pressure state is recomputed, not serialized. Mutexes,
condition variables and waiting thread state are never persisted.

Spool records are immutable after append. Before committing metadata, persistence
syncs dirty spool files and new directory entries **outside the crawl mutex**.
Only then can a snapshot replacement or SQLite transaction publish their offsets,
seen/work changes, counters and completion flag. If an exception or process loss
happens first, the prior metadata still points at valid earlier byte prefixes.
An uncommitted suffix is ignored. Resume validates referenced prefixes and forks
them into a new companion directory; it truncates uncommitted suffixes in the copy.
This also lets separate snapshot recoveries avoid modifying each other's files.

A bounded checkpoint/database with deferred work is therefore a **recovery bundle**,
not a standalone file. Preserve its referenced `*.discovery-*` directory. The current
manifest uses absolute paths: relocating a bundle requires retaining those paths;
no relocation/export command is supplied. Missing, truncated, noncanonical,
wrong-origin/depth, out-of-policy or inconsistent-count prefixes fail before network
requests. Completed metadata with no deferred extents no longer depends on those
files, but older recovery points may still reference them. Persistent companion
logs are retained; remove them offline only after retiring every referencing
recovery point. No-persistence spools are temporary and cleaned up on teardown.

SQLite's dirty journal does not store the large deferred URL list. Fixed admission
quanta bound mutation production per coordinator pass; threshold flushes and the
existing synchronous ordered writer slow consumers when commits lag. Each threaded
worker can have at most one outstanding persistence write, so queued batches are
bounded by worker count (async/sequential: one). Batch thresholds are triggers, not
exact maximum batch sizes; lifecycle events and a completion group can overshoot.
Mutation-buffer and queued-batch peaks are exported and checked in stress runs.
No dedicated writer thread or unlimited background write queue was added.

Metrics distinguish queued peak, reservation peak, seen peak, deferred candidate
peak/current count, admission count, pressure transitions and pressure wall time.
Deferred spool service time includes temporary dedup and shared spool I/O; it can
overlap extraction and persistence timing. Persistence commit time includes the
companion durability fence. These overlapping wall durations must not be summed.
Original network benchmark defaults explicitly disable admission, preserving its
CSV and headline workload; enabled library benchmark reports add watermark/pressure
columns. The separate high-fanout-backpressure tool always records the configuration.

## Release measurements

Darwin arm64, Release, local filesystem, deterministic three-origin graph with
20 branches ×500 leaves +seed =10,021 logical URLs, depth 2, no artificial delay.
One warmup and three measured trials per configuration, fresh crawler/servers/files
per trial. External `/usr/bin/time -l` records child process peak RSS in Darwin
bytes. Tables use medians; durations and peaks are independent medians. Decimal MB
is bytes/1,000,000. Elapsed includes process startup, final flush and close; fixture
startup is excluded. RSS measures the crawler, not the fixture servers or system
page cache. These are warm local measurements, not cold-storage guarantees.

The pre-change baseline above used the original HTTP/1.0 fixture. A first large
post-change HTTP/1.0 trial had transport failures in an unbounded async control and
was rejected before CSV export. The paired measurements below consistently use
HTTP/1.1 +TCP_NODELAY to reduce connection churn; no failed samples are included.
No claim is made about the precise cause of those earlier transport failures.

Full async crawl: configured concurrency 8, host cap 4, robots ignored, no retries,
no persistence. Every sample verifies the complete canonical graph, exact request
counts, absence of duplicates, page budget and host caps.

| High/low | Peak queued | Peak reservations | Elapsed ms | Requests/s | Peak RSS MB | Activations | Pressure ms | Deferred I/O ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Disabled | 10,000 | 10,000 | 384.927 | 26033.533 | 23.888 | 0 | 0 | 0 |
| 1,000/750 | 996 | 1,000 | 465.437 | 21530.323 | 22.118 | 36 | 348.886 | 69.408 |
| 100/50 | 97 | 100 | 443.271 | 22606.932 | 22.004 | 199 | 400.607 | 72.992 |

All runs finish with seen=10,021, pending=0, deferred=0. Peak deferred candidates
were 9,007 and 9,919 for the bounded settings. The 1,000 bound cuts peak queued work
by about 90%, but elapsed grows about 21% and full-crawl RSS improves only modestly.
Seen strings and retained attempt records survive completion and dominate more of
the memory by then. A smaller bound is not monotonically worse or better; these
results do not identify a universal optimum.

Early-growth comparison: same graph, sequential, stop at the original immutable
21-attempt budget (seed and branches). This isolates discovery outrunning leaf
fetches. It is intentionally an incomplete crawl, with retained disk candidates.

| High/low | Peak queued | Peak seen | Elapsed ms | Peak RSS MB | Pressure ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Disabled | 10,000 | 10,021 | 41.238 | 13.091 | 0 |
| 1,000/750 | 1,000 | 1,009 | 89.816 | 8.864 | 45.268 |
| 100/50 | 100 | 102 | 88.923 | 9.208 | 74.254 |

The bound delays seen insertion, as intended. Pending peaks are not necessarily
final counts, and peak seen need not equal high+final completed count because
hysteresis can stop admission before the last fetches finish. At this early stop,
1,000 reservations reduce median peak RSS about 32%. The 100-bound RSS is higher
than 1,000 here; allocator/process noise and fixed buffers make RSS non-monotonic.

SQLite +robots +retries: async concurrency 8, host cap 4, DB batch threshold 100.
One hundred leaves return 503 once; each then succeeds. Each run finishes 10,021
logical resources in exactly 10,121 resource attempts plus three robots fetches.
Every durable final DB has the exact seen set, zero unfinished rows and completed=1.

| High/low | Peak queued | Elapsed ms | Attempts/s | RSS MB | Commit ms | Peak buffered mutations | Peak queued batches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Disabled | 10,000 | 582.702 | 17369.085 | 31.179 | 209.937 | 6,016 | 1 |
| 1,000/750 | 997 | 671.697 | 15067.817 | 24.347 | 201.857 | 1,272 | 1 |
| 100/50 | 97 | 600.672 | 16849.461 | 23.183 | 176.574 | 259 | 1 |

Bounded pressure times were 491.421 and 550.104 ms; deferred I/O service times were
74.726 and 71.045 ms. Commit time includes the spool fence and overlaps parts of
deferred I/O. The closed main DB sizes were 1,966,080 /622,592 /495,616 bytes;
these exclude retained companion logs and must not be interpreted as total disk
usage. DB batching remains synchronous and materially contributes to this tiny
local workload; moving commits off the event loop is a future optimization, not
part of this phase. Queue bounds remain the primary result, not a throughput win.

Reproduce (fresh output paths):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
python3 tools/backpressure_benchmark.py --engine async --runs 3 --warmups 1 \
  --output results/step16-full.csv
python3 tools/backpressure_benchmark.py --engine sequential --budget 21 \
  --runs 3 --warmups 1 --output results/step16-early.csv
python3 tools/backpressure_benchmark.py --engine async --sqlite --robots \
  --retry-every 100 --runs 3 --warmups 1 --output results/step16-sqlite.csv
python3 tests/backpressure_stress.py ./build-release/fluxcrawl
```

## Correctness, stress and manual verification

- Before implementation: 64/64 existing tests passed, 55.14 s.
- Standard final implementation: 66/66 passed, 63.91 s.
- ASan +UBSan: 66/66 passed, 83.09 s. Leak detection is disabled because it is
  unsupported by the macOS sanitizer; halt-on-error is enabled.
- TSan: 4/4 selected tests passed, 60.07 s: threaded crawler, SQLite stress,
  pressure unit tests and pressure integration. Final HTTP/1.1/large-document
  integration was also rerun under TSan: 2/2 passed, 50.27 s.
- Final expanded pressure/benchmark tests: 3/3 passed normally and under ASan/UBSan.
  System libraries are not sanitizer-instrumented.

Unit tests deterministically cover 10/5 hysteresis, transition/time counting,
invalid bounds, capacity-before-seen atomicity, fair origin service, FIFO before
pressure, immutable prefix forks and suffix exclusion, corrupt counts/origins,
100-origin descriptor-cache reuse, callback target equivalence and disk uniqueness,
legacy format-one compatibility and version-two manifest round trips.

Integration tests run every engine with 20/10 and 10/5 bounds, exact unrestricted
URL-set equivalence, cross-page dedup, retry reservation, robots bootstrap, depth
barriers, mixed-origin fairness, and invalid CLI values. A single 2,000-link response
with a 20 limit leaves exactly 20 accepted pending URLs and 1,980 unseen deferred
candidates at the one-attempt stop in all three engines. Tests resume SQLite
threaded→async and snapshot async→threaded while candidates are deferred. Another
case kills a threaded DB owner after a committed deferred prefix and resumes async;
the full URL set completes, with the seed fetched once. A corrupted companion fails
before new requests. Enabled benchmark metadata is tested while preserving the
legacy CSV shape when admission is disabled.

Heavy opt-in stress stays outside default CTest. Threaded and async each process
10,021 logical URLs, three origins, 100 retries, robots enabled, SQLite enabled,
8 workers/transfers, high/low=100/50, batch=100, and exact max_pages=10,121:

| Engine | Peak queued | Peak reservations | Peak deferred | Activations/resumptions | Buffered mutations peak | Queued batches peak |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| Threaded | 94 | 100 | 9,919 | 199/199 | 333 | 2 |
| Async | 97 | 100 | 9,919 | 199/199 | 266 | 1 |

Both have zero fetch/persistence failures, all 100 retries recovered, exact final
seen counts, no lost/extra logical resources and no duplicate fetches beyond the
intentional retries. SQLite foreign keys, empty unfinished state, completion marker,
mutation/batch peaks and server host caps are checked programmatically.

A separate manual Release run used async concurrency 16, host cap 4, high=1,000,
low=750, depth 2, max_pages=10,000 on ten branches ×500 leaves +seed. Actual summary:

```text
Pages attempted: 5011
Responses: 5011
Fetch failures: 0
URLs discovered: 5011
URLs pending: 0
Peak pending URLs: 990
Peak admitted unfinished: 1000
Deferred discovery URLs: 0
Peak deferred discoveries: 4360
Deferred admissions: 5010
Backpressure activations: 16
Admission resumptions: 16
Deferred spool I/O time ms: 33.1604
Time under pressure ms: 133.813
```

Server instrumentation confirmed all 5,011 logical resources exactly once.
Generated CSV, DB/checkpoint/spool artifacts and profiler output stay ignored.

The next memory-scaling bottleneck is the retained exact seen set and attempt
record history; origin/depth metadata also remains proportional to distinct
partitions. No numeric heap breakdown is claimed. A disk quota/compaction and
portable recovery-bundle export are separate follow-up storage concerns. No Bloom
filter, disk-only global dedup, distributed crawler or frontend was added.
