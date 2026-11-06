# Scalable exact deduplication

## Pre-implementation MemoryExact baseline

Release, Darwin arm64, three fresh processes per point. Existing unordered_set<Url>,
canonical https://host.test/page/N URLs generated individually outside each timed
operation. RSS includes the process and a transient URL, not a prebuilt input vector.
100k lookups per sample, 50% hits. Per-operation steady-clock sampling adds overhead.
Reserve time is separate; growth rehashes exclude initial reserve.

| URLs | Reserve | Insert ms | Lookup 100k ms | RSS bytes | Buckets | Load | Growth rehashes |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10000 | 0 | 0.919 | 4.662 | 5095424 | 12853 | 0.778 | 13 |
| 10000 | 1 | 0.868 | 4.962 | 4964352 | 10007 | 0.999 | 0 |
| 100000 | 0 | 20.055 | 12.591 | 25673728 | 102877 | 0.972 | 16 |
| 100000 | 1 | 15.694 | 16.069 | 24625152 | 100003 | 1.000 | 0 |
| 500000 | 0 | 158.119 | 18.158 | 121618432 | 823117 | 0.607 | 19 |
| 500000 | 1 | 86.642 | 17.056 | 112279552 | 500009 | 1.000 | 0 |
| 1000000 | 0 | 370.023 | 22.813 | 240484352 | 1646237 | 0.607 | 20 |
| 1000000 | 1 | 216.198 | 26.024 | 222183424 | 1000003 | 1.000 | 0 |

## Exact membership contract

MemoryExact remains the default. `SeenSet` is coordinator-confined and exposes
insert-if-absent, contains, size, rollback of uncommitted acceptance, diagnostics,
and capture acknowledgement. Discovery uses one insert-if-absent operation after
admission capacity is secured. URL identity is still the canonical Url string;
no normalization or persisted std::hash identity is introduced.

MemoryExact owns the existing unordered_set<Url>. `--seen-reserve N` is explicit:
the baseline shows fewer growth rehashes and faster insertion at large sizes, but
lookup did not consistently improve. Default crawls do not allocate for a guessed
million-element set. Memory mode retains its cheap advisory contains check before spooling to avoid
writing already-known duplicates; final admission still uses one atomic
insert-if-absent. This is a different publication boundary, not an unprotected
contains-then-insert acceptance race. Snapshot/MemoryExact restoration reconstructs the
exact set, reserving known restored capacity.

SQLiteExact uses the existing crawl DB and a reused bound `SELECT 1 FROM seen_urls
WHERE url=?1`. It does not own a second database or per-worker connections. Newly
accepted URLs enter a small exact uncommitted map, then the normal journal records
seen+unfinished/terminal work and counters atomically. That map recognizes duplicates
before COMMIT. Captures tag entries with their ordered transaction sequence;
coordinator operations evict entries once the store's atomic committed sequence
covers them. Size is a logical counter, not a per-call COUNT scan. The cache is
bounded by configured batching, admission quanta and outstanding worker batches,
not by the full durable seen count. Choosing an enormous batch intentionally grows
this durability window/cache; it is not a fixed independent memory ceiling.

The exact table's primary key is a second uniqueness boundary. The existing strict
INSERT remains inside its work/counter transaction. No standalone per-URL durable
insert, silent INSERT IGNORE of a membership bug, or seen-only transaction was
introduced. Prepared membership reads plus a pending cache are the initial exact
reference; a Bloom prefilter can remove many negative reads. Combining direct
unique inserts with batching would still need to preserve unfinished work and
uncommitted membership, so no speculative SQL upsert rewrite was added.

One internal store mutex serializes prepared reads, scans, metrics and writer
transactions on the existing connection. Lookup may hold the crawl coordinator
while waiting for this mutex/I/O to preserve acceptance atomicity. Transaction
writers never acquire the coordinator while holding the store mutex, so there is
no inverse lock order. Writes and deferred-file durability fences remain outside
the crawl mutex. Bloom words and uncommitted-cache mutations use the existing
coordinator boundary; no atomic-word Bloom implementation or backend public mutex
is needed. Async membership reads are synchronous and can pause its event loop.

Bounded admission still precedes insertion. Saturated candidates remain unseen in
the Step 16 disk spool. SQLite modes defer candidates without an eager membership
read at publication, then check exactly once at admission; this avoids a redundant
SELECT before the later insert-if-absent. Cross-page duplicates may therefore
occupy disk until admission. Existing logical retries bypass new-discovery dedup;
robots-blocked resources remain exact seen entries after restart.

## Bloom construction and correctness

For design count n and target probability p, use

```text
m = ceil(-n * ln(p) / (ln(2)^2)) bits
k = max(1, round((m/n) * ln(2))) probes
```

The implementation allocates at least 64 bits for tiny filters, with ceil(m/64)
uint64 words. Invalid n=0, nonfinite/out-of-range probabilities and overflowing
bit-array sizes are rejected before allocation. Allocation failures remain errors.
Probe positions use defined unsigned 64-bit double hashing, `(h1 + i*h2) mod m`,
with wrap modulo 2^64 in the hash accumulator. Two fixed seeds feed explicit
FNV-1a byte loops followed by fmix64 avalanche; the second step is forced odd.
This is a compact experimental hash choice, not a cryptographic or adversarial
collision-resistance claim. Occupancy is popcounted only when reporting.

The sizing relationship and double-hashing approach follow [Kirsch and
Mitzenmacher, Less Hashing, Same Performance](https://www.eecs.harvard.edu/~michaelm/postscripts/esa2006a.pdf).
Measured results below test this particular finite-size/hash implementation; the
formula's nominal probability is not a guarantee for arbitrary URL distributions.

Bloom+SQLite is exact overall:

```text
Bloom negative -> exact insertion/cache path without a SQLite SELECT
Bloom maybe    -> exact pending-cache/SQLite verification
exact absent   -> accept normally, even after a Bloom false positive
```

A positive filter result never rejects a URL by itself. Rollback may leave extra
positive bits; these cause more exact checks, not lost work. There is no lossy
BloomOnly strategy. Bloom+Memory is not exposed because it retains the full RAM
set and adds filter memory; that is not this phase's scalability objective.

Bloom configuration includes expected count and target probability. Diagnostics
show actual insertions, bit occupancy, probe count and capacity exceeded. Overfill
is reported once in the aggregate summary, not once per URL. A saturated filter
continues to give exact crawl results because every positive is verified; its
performance can converge toward exact DB checks plus Bloom overhead.

## Recovery and compatibility

Payload format 3 records dedup mode, design count/probability and optional memory
reserve. Formats 1 and 2 remain readable as MemoryExact. SQLite schema version stays
1 because its exact table and work tables are unchanged. Saved backend/configuration
is retained on resume; strategy switching is not a resume CLI feature.

MemoryExact snapshots and SQLite recovery retain their normal behavior. SQLiteExact
and Bloom+SQLite require `--state-db` and reject snapshots/no-persistence combinations.
There is no temporary authoritative global membership DB and no silent fallback to
BloomOnly or memory if the exact database fails.

SQLite-backed modes load only metadata and bounded unfinished work into the recovery
model, plus an external seen-count descriptor. The DB seen scan validates canonical
URLs, seed presence, policy constraints and count without retaining a vector or set.
Foreign-key/unfinished validation preserves work membership. Snapshot serialization
cannot mistake an external descriptor for a complete seen set. Bloom mode then
streams exact DB rows through a fresh filter before any request starts. No bitset,
hash state or uncommitted cache is persisted; rebuilding is O(durable seen count)
startup work with bounded auxiliary RAM. CLI resume retains the owned connection
from its first load instead of reopening and duplicating whole-state recovery.
Step 16 companion-file requirements still apply to deferred candidates.

Metrics separate lookup and insert-service time, DB membership reads, committed
seen inserts/constraint conflicts, pending-cache hits/peaks, Bloom queries/negatives/
maybes/false positives, occupancy, rebuild and startup time. Memory insert time
includes its combined hash lookup+insert; SQL/Bloom insertion separates exact
lookup from cache/filter insertion. Durability cost remains persistence time.
Confirmed duplicates count rejected insertion attempts, not every positive lookup.
False-positive rate for disjoint synthetic absent queries uses false positives /
absent queries. False positives / verified maybe queries is a different diagnostic
conditional ratio. Neither is the configured target. Bloom definite-negative rate
is negatives / total Bloom queries; DB reads avoided are compared with plain SQL
under the same workload, because pending-cache hits also avoid reads.

## Synthetic Release comparison

Darwin arm64, three fresh processes per configuration; external `/usr/bin/time -l`
RSS in bytes. URLs are generated individually, not retained as an input vector.
The workload uses sequential canonical numeric URL IDs and warm local OS caches.
RSS covers population, three lookup mixes and reopening/rebuild in one process.
It is whole-process memory, not an allocator/heap breakdown. Decimal MB is bytes/1e6.

Memory rows are nondurable exact membership only. SQL rows use the real WAL/FULL
store, 100 new URLs per transaction, with explicit synthetic terminal transitions
and matching counters. They persist seen+terminal outcome atomically, not one
uncoupled seen insert. No HTTP runs in this microbenchmark. Insert-call time excludes
URL generation and durable commits; population time includes generation, batching
and commits, after initialization. These API rows include metrics/virtual dispatch
and are not identical operations to the pre-refactor raw-set baseline above.

| Seen | Strategy | Insert calls ms | Population ms | RSS MB | Closed main DB MB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100000 | memory | 18.773 | 78.535 | 27.820 | 0.000 |
| 100000 | memory-reserve | 15.426 | 75.669 | 26.903 | 0.000 |
| 100000 | sqlite | 253.988 | 571.093 | 10.387 | 3.650 |
| 100000 | bloom-sqlite | 34.381 | 337.801 | 10.584 | 3.650 |
| 500000 | memory | 193.995 | 500.646 | 123.961 | 0.000 |
| 500000 | memory-reserve | 120.900 | 433.105 | 114.606 | 0.000 |
| 500000 | sqlite | 1351.629 | 2916.845 | 15.630 | 18.625 |
| 500000 | bloom-sqlite | 179.512 | 1767.224 | 16.679 | 18.625 |
| 1000000 | memory | 452.751 | 1095.555 | 242.778 | 0.000 |
| 1000000 | memory-reserve | 287.522 | 938.018 | 224.313 | 0.000 |
| 1000000 | sqlite | 2756.468 | 5900.616 | 15.614 | 37.343 |
| 1000000 | bloom-sqlite | 361.056 | 3487.195 | 17.990 | 37.343 |

At 1m, RSS/URL in the original baseline is approximately 240 bytes without
reserve and 222 with reserve, including fixed process overhead. It is not exact
structure attribution. The new API population rates at 1m are approximately
913k URLs/s memory, 1,066k reserved memory, 169k SQL and 287k Bloom+SQL. SQL durability
makes these different contracts; the crawl comparison below holds persistence fixed.
Scalable synthetic cache peaks were 100 entries. Bloom bit arrays were 119,816 /
599,072 /1,198,136 bytes at 100k /500k /1m design counts, target 1%. SQL main-file
sizes exclude auxiliary journal/ownership files; no full in-memory seen copy is
materialized in either scalable restore path.

## Lookup mix and probability experiments

Each phase makes 100,000 exact queries after population, with inputs constructed
outside the timed call. New means disjoint/absent; lookups do not insert those
inputs. At 1m seen, with a correctly sized 1% filter:

| New / present | Memory ms | SQLite ms | Bloom+SQLite ms | SQL reads | Bloom SQL reads | Bloom false positives |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 95/5 | 31.622 | 255.145 | 32.101 | 100000 | 5921 | 921 |
| 20/80 | 47.274 | 362.777 | 323.901 | 100000 | 80195 | 195 |
| 50/50 | 41.498 | 325.292 | 208.467 | 100000 | 50478 | 478 |

The 95%-new case avoids 94.079% of plain SQL reads and gives 94.079% definite
negatives. The 80%-present case avoids only 19.805% of reads: actual duplicates
still require exact verification. Filter hashing adds overhead and cannot eliminate
positive DB checks. The 95%-new lookup rates were about 3.16m/s memory, 392k/s SQL
and 3.12m/s Bloom+SQL on this warm local dataset.

Probability sweep: design and actual count 100k; 95k absent +5k present queries.
Observed FP is false positives /95,000 absent queries, not false positives /maybes.

| Target FP | Filter bytes | Probes | False positives | Observed FP | Exact DB reads | Queries/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10% | 59912 | 3 | 9478 | 9.9768% | 14478 | 2157888 |
| 1% | 119816 | 7 | 906 | 0.9537% | 5906 | 3830945 |
| 0.1% | 179720 | 10 | 103 | 0.1084% | 5103 | 4094279 |

Lower target probability uses more bits/probes and avoids more unnecessary reads.
These are measured finite samples, not exact statistical probability assertions.

A fixed 100k-capacity, 1% filter was also overfilled. The same 95k-absent query
denominator is used; all exact membership assertions still passed:

| Actual insertions | Filter bytes | Bit occupancy | Observed FP | Exact DB reads |
| ---: | ---: | ---: | ---: | ---: |
| 100000 | 119816 | 51.806% | 0.954% | 5906 |
| 200000 | 119816 | 76.831% | 15.966% | 20168 |
| 500000 | 119816 | 97.434% | 83.306% | 84141 |

The nominal 1% target plainly does not survive 5x overfill. Exact results do:
false positives increase DB work rather than suppressing valid URLs.

## Startup measurements

Close, reopen, validate and build the backend in the synthetic process. SQL mode
streams canonical validation without a seen vector/set. Bloom then performs a
second streaming scan to rebuild the bitset. There are no unfinished synthetic
resources to restore to the scheduler here. All samples validate the external seen
count and query known restored URLs. Component medians need not sum to totals.

| Seen | Mode | Open ms | Load/validation ms | Bloom rebuild ms | Total startup ms |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100000 | sqlite | 0.386 | 69.031 | 0.000 | 69.415 |
| 100000 | bloom-sqlite | 0.394 | 70.251 | 71.324 | 141.958 |
| 500000 | sqlite | 0.384 | 357.788 | 0.000 | 358.165 |
| 500000 | bloom-sqlite | 0.370 | 358.786 | 388.518 | 749.035 |
| 1000000 | sqlite | 0.425 | 722.080 | 0.000 | 722.506 |
| 1000000 | bloom-sqlite | 0.398 | 734.294 | 772.696 | 1517.006 |

This is process-equivalent backend startup with warm filesystem caches, not a
cold-machine benchmark. Rebuilding avoids a Bloom file/hash-compatibility protocol
but adds O(seen) work: about 773 ms beyond validation at 1m.

## Fixed-engine real crawl

Final Release implementation, Step 16 graph: seed +20 branches ×500 leaves
=10,021 canonical resources. Async concurrency 8, host cap 4, high/low=1,000/750,
robots ignored, retries disabled, SQLite WAL/FULL batch threshold 100 for **every**
strategy, including MemoryExact. One warmup and three measured fresh processes per
strategy. HTTP/1.1 fixture with TCP_NODELAY. All samples validate the exact graph,
request counts, queue/host caps, foreign keys and durable completion. Elapsed is
whole process, including initialization/final flush/close, excluding fixture startup.

| Strategy | Elapsed ms | Requests/s | Peak queued | RSS MB | Dedup lookup ms | Insert service ms | DB reads | Bloom FP |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| memory | 628.014 | 15956.646 | 996 | 23.691 | 0.346 | 2.331 | 0 | 0 |
| sqlite | 613.970 | 16321.637 | 996 | 22.020 | 19.758 | 0.626 | 10020 | 0 |
| bloom-sqlite | 602.802 | 16624.034 | 996 | 22.004 | 0.682 | 1.159 | 22 | 22 |

Bloom design count was 10,021, target 1%, bit array 12,008 bytes. Median definite
negatives were 9,998 of 10,020 queries (99.7804%); only 22 new URLs required exact
negative DB verification. These crawl false positives occur during progressive
population, so their ratio is not the final-capacity disjoint-query probability.
Both scalable modes peaked at 512 uncommitted identities and finished with none.
All modes peaked at 1,000 admitted unfinished reservations.

Persistence commit medians were 200.025 /191.161 /194.896 ms for memory /SQL /Bloom.
The elapsed medians are close, with substantial per-trial variation: memory
584.845–758.695 ms, SQL 599.447–631.355 ms, Bloom 589.564–620.108 ms. This small
shared-host sample does not establish a reliable throughput winner. A prior
pre-edge-case-fix run also had close medians but a different ordering; the table
uses the final rerun. Reducing DB reads does not eliminate HTML, network, spool,
record retention or transactional costs. Full-crawl RSS savings are modest here
because the graph is small and response/attempt records remain in RAM.

## Stress, actual resume and verification

The opt-in heavy run uses 100 branches ×1,000 leaves +seed =100,101 URLs,
async concurrency 8, host cap 4, high/low=1,000/750, batch=100, Bloom design
100,101 and target 1%. It stops cleanly at 32 attempts, keeps the same servers,
restarts, rebuilds Bloom and finishes within the original 100,101 attempt budget.
Final-run measurements:

```text
logical URLs / resource attempts / responses: 100101 / 100101 / 100101
peak queued / admitted unfinished: 1000 / 1000
uncommitted seen cache peak / remaining: 253 / 0
SQLite buffered mutations peak / queued batches peak: 2035 / 1
SQLite seen constraint conflicts / persistence errors: 0 / 0
Bloom rebuild ms: 0.968792
DB open ms: 1.05796
DB validation ms: 62.2336
whole resume startup ms: 126.763
resume process elapsed ms: 5613.438
Bloom queries / definite negatives / false positives: 99088 / 98916 / 172
Bloom bytes / insertions / bit occupancy: 119936 / 100101 / 51.8352%
final unfinished / deferred candidates: 0 / 0
durable completed: true
```

Server counts verify every canonical resource exactly once, no loss/duplicate
fetches, and host caps. SQLite row counts, foreign keys and completion are inspected
programmatically. Startup includes CLI open/validation, Bloom rebuild, scheduler
restore and deferred-prefix validation/forking. In this partial graph the exact
DB held only 1,013 seen rows before restart; deferred-prefix checks dominate
startup, including repeated validation in the shared recovery path. This explains
why its 127 ms total is much larger than its 1 ms Bloom rebuild. The synthetic
large-DB table above separately measures 100k–1m-row rebuild scaling.

Verification evidence:

- Unchanged baseline suite: 66/66 passed (59.48 s).
- Final standard suite: 68/68 passed (68.88 s). Release configure/build succeeded.
- ASan+UBSan full suite: 68/68 passed (87.34 s); final disabled-cap scalable
  edge cases were rerun: 2/2 passed (10.51 s). macOS leak detection is unsupported
  and disabled; halt-on-error is enabled.
- TSan selected suite: 4/4 passed (82.48 s): threaded crawler, SQLite stress,
  dedup unit and dedup integration. Final expanded dedup tests: 2/2 passed
  (76.39 s). System libraries are not sanitizer-instrumented.
- Initial concurrent verification had timeouts in legacy benchmark/threaded
  harnesses. Isolated reruns succeeded, followed by the full successful runs above.
  No timeout was treated as a performance sample or hidden as a pass.

Unit tests cover sizing, invalid/overflowing inputs, stable hash golden value,
10k inserted keys with no false negatives, broad deterministic FP sanity bounds,
canonical/query identity, exact duplicate insertion, pre-commit cache membership,
SQL failure rollback, uniqueness/conflict accounting, stale Bloom positives,
forced tiny-filter false-positive acceptance and a 100-row streaming rebuild.

Integration covers all three backends on all three engines, cross-page/concurrent
duplicates with a batch larger than the graph, tiny watermarks, retries, robots
bootstrap and permanently blocked seen identities. Threaded→async clean recovery
and committed-prefix crash recovery preserve the exact final set. Capacity-disabled
SQL/Bloom mode also tests 500 links from one page: only 64 new identities enter
the first uncommitted cache publication, with the remainder deferred; restart
completes the entire graph. Malformed CLI/backend combinations fail safely.

Reproduce with fresh output names:

```sh
cmake -S . -B build && cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
python3 tools/dedup_experiment.py --runs 3 --output results/step17-synthetic.csv
python3 tools/dedup_crawl_benchmark.py --runs 3 --warmups 1 \
  --output results/step17-crawl-final.csv
python3 tests/dedup_stress.py ./build-release/fluxcrawl
# Heavy CTest registration is optional: -DFLUXCRAWL_SLOW_TESTS=ON
```

The external RSS wrapper in the experiment driver is Darwin-specific; no such
platform-specific measurement code was added to the production crawler. Raw CSVs,
DBs, companion files, journals and traces remain ignored.

## Recommendation and next bottleneck

Keep MemoryExact for ordinary/smaller crawls: exact, simple, low membership latency.
Reserve only when a credible expected count exists; it helps bulk insertion and
large-set RSS here but front-loads allocation and did not consistently improve
lookup latency. Use SQLiteExact when keeping the full global set off RAM matters
and a bitset/capacity estimate is undesirable. Use Bloom+SQLite for large persistent
crawls dominated by new URLs when its bit budget and O(seen) startup are acceptable.
Duplicate-heavy streams retain most SQL checks. These data do not establish an
automatic scale crossover or justify changing the normal default.

The next major memory source is retained attempt/response record history, followed
by host/partition metadata and active response/DOM buffers. Scalable seen membership
does not bound all process memory. Synchronous commits still cost about 1.8 s in
the 100k resume process, and full canonical validation/Bloom rebuild remains linear
in durable seen count. Deferred spool compaction/quota and repeated prefix
validation are separate storage/startup concerns. No asynchronous DB I/O, lossy
dedup, Cuckoo filter, sharding, distributed crawler or frontend was added.
