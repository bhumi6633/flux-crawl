# A URL through FluxCrawl

This walkthrough follows the current default bounded-admission path. “Async”
means `AsyncCrawler`, the event-driven engine. “Pending” means queued resource
work, including delayed retries and robots-held resources; active requests are
separate. A deferred candidate on disk is not yet an accepted identity.

## 1. Discover a candidate

`Url` parses/resolves HTTP(S) references, removes fragments and normalizes default
ports while preserving queries. `HtmlLinkExtractor` uses libxml2, filters anchors
and resolves against the response's effective URL. Shared processing applies
crawl depth and optional same-host filtering. Redirects can change the base URL;
they do not turn each curl redirect hop into a separately scheduled crawl item.

In bounded mode, extraction streams through a disposable SQLite per-document
index instead of retaining a document-sized URL vector. It preserves first
occurrence order and per-document exact deduplication. The vector extractor
remains available in unbounded MemoryExact mode. This distinction matters when
explaining the older vector-extractor optimization measurements.

Evidence: [URL](../src/url.cpp), [HTML extraction](../src/html_parser.cpp),
[shared processing](../src/crawl_processing.cpp), parser and adversarial tests.

## 2. Defer, then accept exactly once

`CrawlState::defer` appends candidates to `DiscoverySpool`, partitioned by depth
and origin. Deferred duplicates can exist; they do not imply duplicate logical
acceptance. Admission checks the high/low gate and exact seen membership before
moving a candidate into accepted work. Before pressure, publication order is FIFO;
after pressure, shallowest-depth origin rotation helps distribute admission.
Admission scans/yields in bounded batches rather than monopolizing coordination.

The hard bound covers queued ordinary work. The stronger reservation bound also
includes active ordinary requests, leaving room to return a failed request as a
retry. Robots infrastructure is exempt so policy resolution cannot deadlock.
Held ordinary URLs still count. Tiny bounds can therefore reduce useful transfer
concurrency. Depth-limited candidates are rejected before acceptance; a budget
stop may leave unseen deferred candidates recoverable on disk.

Threaded producers **do not wait for admission capacity**. They finish disk
publication and return to consuming; otherwise every worker could become a
blocked producer. The async engine likewise defers discovery without waiting for
capacity. Disk I/O and parsing still take time on the event loop; “does not wait
for capacity” does not mean every operation is nonblocking.

Evidence: [CrawlState](../src/crawl_state.cpp), [spool](../src/discovery_spool.cpp),
[admission contract](backpressure.md), backpressure parity/stress tests.

## 3. Separate permission from eligibility

`CrawlPolicy` answers whether an accepted URL may be fetched. With robots enabled,
it holds ordinary resources while one origin bootstrap resolves. The cache is
keyed by normalized scheme/host/effective port. Successful policy responses and
404/410 permit rule evaluation; unavailable bootstrap outcomes conservatively
deny that origin after applicable retry handling. Rules implement the documented
literal-prefix subset, not full robots standards compliance.

`HostScheduler` answers when work may run: depth barrier, per-origin capacity,
token availability and retry deadline. Round-robin origin queues provide fairness
among eligible origins, not a universal latency guarantee. A concurrency cap
bounds simultaneous claims; a token bucket bounds start rate and permits a
configured burst. One cannot replace the other. Robots and retries use these
same scheduling constraints. Curl redirect destinations do not obtain new leases.

Evidence: [policy](../src/crawl_policy.cpp), [scheduler](../src/host_scheduler.cpp),
[robots](robots.md), [host scheduling](host-scheduling.md).

## 4. Reserve and execute

A scheduler lease owns an active claim and releases it through RAII. Resource
attempt budget is charged at reservation, so multiple workers cannot overshoot
`max_pages`. Retries consume that budget; infrastructure robots attempts are
tracked separately. HTTP transport returns typed success/error outcomes and
bounded response bodies; it does not decide global identity.

| Engine | One fetch | Ownership and waiting |
| --- | --- | --- |
| Sequential | claim → blocking fetch/parse → publish → repeat | One crawl thread; optional sampler reads under synchronization |
| Threaded | worker claims → fetch/parse outside lock → publish | Coordinator mutex protects mutation; condition-variable waits release it |
| Async | launch → perform/poll → completion → publish → refill | One crawl owner; concurrent transport, serialized completion processing |

The threaded coordinator cannot finish merely because its queue is momentarily
empty: another active worker can publish links. It checks active work and future
eligibility, waits until notifications/deadlines and rechecks predicates.
Stop/budget/fatal-error paths are distinct from exhausting all reachable work.

The async engine owns a map from easy-handle pointers to `unique_ptr` transfer
objects. Heap allocation keeps callback userdata stable across map rehashes.
Completion code copies `CURLMSG_DONE` result data before removing the handle,
finishes the transfer, and keeps its lease through publication. Destruction
removes handles before destroying callback state. It drains available completions
then refills slots rather than waiting for an entire batch. When only delayed
work remains, it sleeps until scheduler eligibility; with active transfers it
uses `curl_multi_poll` with scheduler/curl timing constraints. Empty ready work
is not termination while active transfers, delayed retries or deferred candidates
can still progress.

Evidence: [thread pool](../src/thread_pool_crawler.cpp),
[async engine](../src/async_crawler.cpp), [transfer ownership](../src/curl_transfer.hpp),
engine, scheduling, retry and profiling tests.

## 5. Complete, retry or discover

Shared completion processing records outcomes, applies retry classification,
extracts eligible HTML links and publishes logical state. Retriable HTTP statuses
are 429, 500, 502, 503 and 504; selected transport errors also retry. Body limits,
TLS validation errors and redirect loops do not. Exponential backoff is capped;
full jitter is optional. Retry-After can extend the delay. Retry count changes,
URL identity does not. Retryable failure is not terminal until its allowance is
exhausted; sibling work can continue.

Default retries are disabled. HTTP status errors remain response records rather
than fabricated transport failures. Refer to the exact exit-code and classification
rules instead of equating any HTTP response with a successful application result.

Evidence: [completion](../src/crawl_processing.cpp), [retry policy](../src/retry_policy.cpp),
[retry details](retries.md), HTTP chaos and delayed-only idle tests.

## 6. Cross a durable boundary

Snapshots serialize logical state, including unfinished work, retry timing and
seen identity. Temporary-file validation/replacement protects the prior primary;
deferred-discovery companion files must remain available. SQLite instead batches
seen/work/terminal changes and metadata in transactions, exposing a committed
prefix. A failed transaction must not leave a URL durably seen but without its
corresponding work/terminal state. One FluxCrawl owner controls a database.

Sockets, curl internals and mutexes are not serialized. Active work is reissued
from logical state, potentially repeating an HTTP request. Batch size trades
transaction cost for a larger replay window. Resume can select another engine
because the durable schema represents crawl semantics, not engine internals.
This is at-least-once recovery of covered unfinished work, not exactly-once
network delivery or arbitrary filesystem power-loss proof.

MemoryExact stores identities in RAM. SQLiteExact queries exact disk membership
with an uncommitted cache. Bloom+SQLite tests the bitmap first: negative means
absent; maybe requires exact cache/SQLite verification. Insertion/rollback and
rebuild order preserve this role. A Bloom filter with `m` bits, `k` probes and `n`
insertions has approximate false-positive probability `(1-exp(-k*n/m))^k` under
standard hash assumptions. Overfilling raises the lookup cost, not URL loss.
A Bloom-only visited set would break correctness, so no such mode exists.

Evidence: [persistence runtime](../src/persistence_runtime.cpp),
[SQLite](../src/sqlite_store.cpp), [exact membership](../src/seen_set.cpp),
[checkpointing](checkpointing.md), [deduplication](deduplication.md),
engine write-fault tests and cross-engine recovery suites.

## 7. Publish visibility without giving away ownership

Engines copy bounded counters/aggregates into immutable snapshots under their
existing ownership rules. The server reads snapshots; it never traverses a live
scheduler. JSON encoding and WebSocket writes do not hold the crawl mutex. Each
client has one in-flight frame; stale ticks are skipped and slow writes time out.
The frontend validates message structure and bounds history. Telemetry is sampled
visibility, not the durable journal or a control dependency.

Evidence: [snapshot publication](../src/observability.cpp),
[server](../src/observability_server.cpp), [frontend validator](../dashboard/src/schema.ts),
[observability design](observability.md), slow-client and lifecycle tests.

## What remains bounded, and what does not

Admitted unfinished work, active claims, response body limits and telemetry caps
have explicit controls. Retained attempt records, host state, exact memory seen
sets, disk spool and databases can grow. SQLite-backed membership alone does not
make the entire crawler constant-memory. DNS-aware politeness, browser rendering,
HTTP caching and distributed ownership are outside the current implementation.
