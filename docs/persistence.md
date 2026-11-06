# Persistent storage experiments and design

Step 17 adds SQLiteExact and Bloom+SQLite membership. Its format-three payload
retains schema version one; scalable recovery streams validation and does not
materialize the full seen set. [Current dedup design and measurements](deduplication.md)
qualify the default-MemoryExact RAM/startup discussion and historical data below.

Step 16 extends the metadata payload to format two while retaining SQLite schema
version one. Bounded crawls can reference append-only deferred-discovery companion
files; preserve them with the database. See the [current admission and recovery
contract](backpressure.md). Measurements below describe the Step 15 implementation;
the persistence-only benchmark now explicitly passes `--unbounded-pending`.

## Step 14 scaling baseline, before SQLite implementation

Release measurement on the local development machine, three samples per size,
100 unfinished resources, remaining URLs terminally completed, no network traffic.
The existing writer includes validation, serialization, temporary write, flush,
POSIX file fsync, read-back validation and rename. Loads follow writes and thus
exercise warm filesystem caches; these are not cold-disk measurements.

| Seen URLs | Snapshot bytes | Median whole write ms | Median load ms |
| ---: | ---: | ---: | ---: |
| 1,000 | 38,801 | 2.670 | 2.152 |
| 10,000 | 344,907 | 17.794 | 12.141 |
| 100,000 | 3,495,013 | 191.673 | 133.087 |
| 500,000 | 17,895,116 | 1336.706 | 820.739 |

Command (executed before introducing SQLite):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target fluxcrawl_persistence_scale -j 4
./build-release/fluxcrawl_persistence_scale results/step15-baseline \
  1000 10000 100000 500000 > results/step15-snapshot-baseline.csv
```

Every generated/read state was validated. The synthetic URL-construction time is
excluded. Raw CSVs remain ignored in `results/`. This establishes whole-state cost;
future incremental commits are different operations and must not be presented as
like-for-like replacements of full snapshot writes.

## Storage and transition contract

`CrawlMutationSink` observes logical discovery, work-state, terminal and accounting
transitions. `PersistenceRuntime` selects None/Snapshot/Sqlite and owns sequencing,
flushes and diagnostics. `CrawlStore` consumes immutable logical batches;
`SqliteStore` implements them using the SQLite library. URL parsing, HTML extraction
and HTTP value types contain no database code. Snapshot format 1 stays compatible.

Schema version 1 is checked independently in `PRAGMA user_version` and
`crawl_meta.schema_version`, alongside an application ID. Application version is
ordinary crawl metadata, not the schema version. No migration is attempted.

| Table | Durable content |
| --- | --- |
| `crawl_meta(key,value)` | Shared versioned metadata payload: seed, semantic and HTTP options, counters, sequence, completed flag; active robots count |
| `seen_urls(url PRIMARY KEY)` | Canonical URL strings; SQL uniqueness backs up hot exact dedup |
| `unfinished(url PRIMARY KEY REFERENCES seen_urls,...)` | Depth, retry count, recovery flag, ready/delayed/active state, nullable eligible wall milliseconds, queue order |

Unfinished state is mutually exclusive per URL. Origin is reconstructed from URL;
no hashes, bucket layouts, sockets, steady-clock values or host tokens are stored.
Checks constrain flags, nonnegative counts and state/deadline consistency. Indexes
cover queue order and delayed work. All URL values use bound parameters. Connection,
statement and rollback-unless-committed transaction wrappers own SQLite resources.
Prepared seen insert, work upsert/delete/read and metadata upsert statements are
reused with reset/clear-bindings. Duplicate insertion is an error, not silently ignored.

Each ordered transaction contains new seen URLs, their unfinished or explicit
terminal state, coalesced work transitions, counters and its sequence. Completion
removes unfinished work; retry replaces the active row with one delayed row.
Metadata and work commit together, including final completion. Injected failures
between statements roll everything back. A failed final transaction cannot publish
a completed marker. No destructor silently flushes an unsubmitted suffix.

The journal only traverses dirty URLs at flush time. It copies no full seen set and
never reloads it for ordinary commits. Queue capture occurs under the coordinator
mutex; an ordered queue detaches immutable batches. A separate writer mutex lets
any worker drain that queue in capture order, outside the coordinator and queue
locks. SQLite I/O never holds the crawl-state mutex. Async uses the same path,
so commit stalls pause its event loop; timing excludes these pauses from request
latency. Persistence failure stops new claims, drains active work, and reports
failure rather than a falsely durable success. Single-process ownership is a POSIX
advisory sidecar lock; the non-POSIX fallback uses SQLite exclusive locking.
External database writers are unsupported. Diagnostic readers are allowed.

WAL with `synchronous=FULL` is explicitly selected. SQLite documents the WAL commit
sync behavior in [synchronous](https://sqlite.org/pragma.html#pragma_synchronous)
and the journal lifecycle in [WAL](https://sqlite.org/wal.html). These settings do
not make the in-memory buffer durable. The default threshold is 100 logical
mutations, not 100 SQL statements or URLs. Coordinator boundaries and fanout can
overshoot it; dirty work changes coalesce, while counters retain all activity.
There is no timer flush or hard time-bound durability guarantee. Initialization
is durable before network starts. Clean stops and completion force the tail flush.

## Restore and validation

Open requires an existing database, correct application/schema IDs, required
metadata and readable tables. Load checks canonical strings through `Url`, foreign
keys, unique work identities, retry ranges, deadlines, semantic configuration,
aggregate work/counter consistency and completed state. Invalid or unsupported
state fails before requests. SQLite resume has no snapshot one-million-seen cap.

Seen rows rebuild the existing exact in-memory hash set; unfinished rows rebuild
the shared scheduler in queue order. Wall deadlines convert to steady deadlines
using the Step 14 conversion. Active rows become recoverable unfinished resources;
active resource and robots attempts become unresolved cumulative attempts, retaining
the page budget. Robots policy starts Unknown, robots-held resources retain their
saved work/deadlines, and host limiters restart cold. Completed resources remain
seen and do not return to the frontier. Execution engine, worker/transfer count
and batch threshold can change; seed, policy and HTTP semantics cannot.
The CLI detects a DB by its header, reads the recovery model, then runtime reopens
under ownership and checks sequence/seed to reject an intervening state change.
This currently repeats the database load; the primitive restore benchmark below
measures one load/rebuild, not this complete CLI startup path.

## Incremental and restore scaling

Release, SQLite 3.43.2, Darwin arm64, three fresh DB samples per size. Warm local
filesystem, synthetic states with 100 unfinished resources. Initialization includes
validation and all initial rows. A fixed subsequent transaction adds **100 new URLs
and 100 unfinished rows** (200 logical mutations); it is not a whole-state rewrite.
Closed file sizes and restore counts include those added rows. Loads restore 200
unfinished resources. Timings are component medians and need not sum exactly.
These samples ran during verification activity, so small absolute times include
host contention; they establish scaling, not a universal latency guarantee.

| Initial seen | Create ms | Incremental ms | Closed DB bytes | Total single restore ms |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 3.781 | 1.511 | 102,400 | 3.627 |
| 10,000 | 11.035 | 0.973 | 454,656 | 14.082 |
| 100,000 | 87.579 | 1.904 | 4,141,056 | 139.101 |
| 500,000 | 565.432 | 2.421 | 20,926,464 | 904.566 |

| Initial seen | Open/schema ms | Seen read/parse ms | Work read ms | Validate ms | Hot seen rebuild ms | Scheduler ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.431 | 2.022 | 0.504 | 0.385 | 0.144 | 0.021 |
| 10,000 | 0.351 | 10.913 | 0.281 | 1.792 | 1.009 | 0.012 |
| 100,000 | 0.334 | 97.782 | 0.274 | 28.052 | 12.604 | 0.013 |
| 500,000 | 0.329 | 480.401 | 0.269 | 252.602 | 179.277 | 0.012 |

```sh
./build-release/fluxcrawl_persistence_scale --sqlite results/step15-sqlite-scale \
  1000 10000 100000 500000 > results/step15-sqlite-scale.csv
```

The incremental update avoids the snapshot's growing rewrite cost (1,337 ms at
500k), but SQLite startup still grows with the exact seen set. Seen parsing,
validation, hash construction and retained strings are the next measured bottleneck.
Peak memory was not instrumented here; no numeric RAM claim is made. Duplicate
CLI loading is another bounded implementation opportunity. Neither a Bloom filter
nor moving hot dedup to disk is part of this phase.

## Whole-crawl batching experiment

Separate Release process benchmark, four loopback origins with 100 leaf pages
each plus one seed (401 requests), depth 1, global concurrency 16, per-origin cap
4, robots ignored, retries disabled, no artificial delay. Every configuration uses
the same HTTP/1.1 fixture with TCP_NODELAY. Async can reuse connections through
curl_multi; the blocking implementation's easy handles have a different connection
lifecycle. Compare persistence settings within each engine. These tiny, local
process timings include spawn, DB initialization, final flush and close; server
startup is excluded. Commit timings exclude initialization/close. File sizes are
closed main-file bytes, unlike the CLI's live main+WAL bytes. One warmup and three
measured trials per setting; table values are medians. Each trial validates exact
URL starts, one request per resource, counters, completion and concurrency caps.

| Engine | Batch | Elapsed ms | Requests/s | Transactions | Mean logical mutations/tx | Commit ms | Closed bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Threaded | None | 51.868 | 7731.145 | 0 | 0 | 0 | 0 |
| Threaded | 1 | 78.176 | 5129.451 | 402 | 6.980 | 48.768 | 98,304 |
| Threaded | 10 | 57.954 | 6919.291 | 135 | 20.785 | 25.401 | 98,304 |
| Threaded | 100 | 54.671 | 7334.812 | 17 | 165.059 | 9.973 | 98,304 |
| Threaded | 500 | 53.771 | 7457.534 | 5 | 561.200 | 4.776 | 98,304 |
| Async | None | 23.541 | 17034.050 | 0 | 0 | 0 | 0 |
| Async | 1 | 32.929 | 12177.762 | 58 | 48.379 | 13.734 | 98,304 |
| Async | 10 | 31.749 | 12630.203 | 57 | 49.228 | 12.950 | 98,304 |
| Async | 100 | 30.057 | 13341.410 | 17 | 165.059 | 7.763 | 98,304 |
| Async | 500 | 43.314 | 9258.057 | 5 | 561.200 | 7.203 | 98,304 |

```sh
python3 tools/persistence_benchmark.py --engine all --runs 3 --warmups 1 \
  --pages-per-origin 100 --output results/step15-persistence-benchmark.csv
```

Threshold 1 does not mean one SQL operation per transaction: fanout, request
lifecycle transitions and async event-loop completion groups can exceed it. The
final metadata flush counts as a transaction, even with zero logical mutations;
schema/initial population does not. Logical mutations differ from SQL row changes.
Bigger batches reduce commit overhead, but do not monotonically improve whole
process throughput. Async batch 500 was slower than 100 on this workload. The
100 default is a development tradeoff, not a universally optimized threshold.

Two earlier HTTP/1.0, 1,001-resource benchmark attempts were rejected because of
transport failures, including a no-persistence trial. No partial CSV or failed
trial contributes to this table. Reducing connection churn and workload size
produced the fully validated experiment above. It does not establish the precise
cause of those earlier transport failures.

## Verification

Before implementation the complete existing suite passed 61/61. Afterward:

- Standard configure/build: 64/64 tests passed (56.87 s).
- Release configure/build succeeded; all performance data uses Release.
- ASan + UBSan: 64/64 passed (71.12 s), halt-on-error enabled. macOS rejected
  `detect_leaks=1`; rerun used `detect_leaks=0`. Leak detection is not claimed.
- TSan: 5/5 selected tests passed (121.55 s): threaded crawler, legacy checkpoint
  integration, SQLite store, SQLite integration and SQLite stress. System libraries
  are not sanitizer-instrumented.

Store tests inject exceptions between statements, verify rollback and a failed
final completion transaction, exercise repeated bound statements, strict duplicate
constraints, active accounting and delayed transitions, and reject newer schema,
missing metadata, noncanonical URLs, invalid retries and orphan work.
Integration tests cover every engine, sequential→async/threaded→async/async→threaded
resume, completed no-op, page budgets, saved retry deadlines, robots reacquisition,
owner rejection, active crash recovery, loss of buffered mutations and forced clean
flush. Threaded and async stress each finish 2,001 resources with exact row counts,
no duplicate fetches and host/global caps intact.

Reproduce correctness checks with the commands in README and:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure
ctest --test-dir build-tsan --output-on-failure \
  -R 'sqlite|core.thread_pool_crawler|checkpoint.integration'
```

Runtime databases, journals, benchmark CSVs and build output remain ignored.

A separate manual Release crash run used four origins ×100 pages plus seed,
threaded workers 8, batch 100, depth 1 and max_pages 1000. After observing a
committed boundary at 31 attempts with eight active work rows, the process was
stopped and killed. Async concurrency 16 resumed the same file. Final inspection:

```text
schema_version=1 seen=401 unfinished=0 delayed=0 completed=1
pages_attempted=409 responses=401 fetch_failures=0
recovery_attempts=8 unresolved_prior_attempts=8
logical server URL set=401 seed requests=1 observed server requests=411
```

The server's 411 starts exceed the durable 409 attempt count because the killed
process had started work beyond the committed boundary. This directly demonstrates
the buffered durability window and at-least-once behavior; cumulative durable
counters cannot count every network effect lost before commit. The final logical
URL set matches the 401-resource uninterrupted reference graph.
