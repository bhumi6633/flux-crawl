# Design notes

## Same semantics, different request execution

FluxCrawl's comparison is useful only if the engines run the same policy.
`CrawlState` owns exact accepted identities and deferred discovery. `CrawlPolicy`
handles origin robots bootstraps and held resources. `HostScheduler` owns ready,
active and delayed attempts, depth barriers, token buckets and scoped leases.
Shared response processing classifies transport/status errors, decides retries,
extracts links and publishes logical completion.

Sequential execution has one blocking transfer. Threaded execution serializes
shared mutation under a coordinator mutex while each fixed worker blocks on its
own transfer and parses outside the lock. Async execution mutates crawl state on
one event loop and keeps multiple easy handles in a curl multi handle. It refills
slots after draining available completions. Expensive completion/HTML work still
blocks that event loop; async is not a promise of parallel parsing.

The depth barrier prevents a deeper discovery from winning the shortest-depth
identity race while shallower requests remain active. It trades some utilization
for predictable BFS depth semantics. Page budgets are charged at reservation,
including retries, so workers cannot race past `max_pages`. A scheduled retry
keeps the crawl alive even when ready and active counts are zero.

## Identity and admission

Seen means logically accepted, not successfully fetched. A URL can be completed,
blocked, pending or active while remaining seen. Retry state belongs to an attempt
and never changes the URL's canonical identity. Per-document href dedup is an
extraction optimization; global exact membership determines acceptance.

Pressure hysteresis counts admitted unfinished resources, including active ones,
so retry capacity is reserved. A full admission gate sends extracted discovery
to an append-only disk spool. It does not discard links to satisfy a memory graph.
Pending memory is bounded; response bodies, retained records, hosts, durable disk
state and exact memory dedup have separate scaling limits.

MemoryExact is simple and fast but retains the full seen set. SQLite exact mode
keeps membership on disk and tracks a bounded uncommitted seen cache until its
batch commits. A Bloom negative avoids a DB membership query; a maybe hit still
checks exact state. Overfilling a Bloom filter degrades speed and occupancy, not
correctness. There is no probabilistic-only URL dropping mode.

## Durable prefix, not exactly-once fetching

Snapshot state includes unfinished logical work and retry deadlines, plus the
seen set. Its temp file is flushed/synced and reread before replacement; failure
preserves the old primary. A crash can leave an orphan temp file, and directory
entry durability across power loss is filesystem/platform dependent. Deferred
companion files are part of recovery state, not optional scratch files.

SQLite batches discovery, unfinished/terminal mutations and aggregate metadata
inside one transaction. A URL is never committed as seen without a corresponding
unfinished or terminal state. A failed transaction exposes only the previous
committed prefix. Runtime batches and exact-cache pruning are ordered by sequence.
A single owner lock prevents competing FluxCrawl owners on supported POSIX hosts.

Requests active at the durable boundary may run again; completions/retries after
that boundary can be replayed. Logical identity stays exact, but network delivery
is at least once. Resume does not resurrect terminal rows from the committed
prefix, and it cannot exceed the remaining cumulative attempt budget. Corruption
is rejected before an engine starts issuing requests.

## Operational telemetry ownership

Telemetry copies counters and bounded aggregates under the engine's existing
ownership. Only immutable snapshots cross into the HTTP thread. JSON work and
slow-client writes never hold a scheduler lock. Blocking-engine samplers join
before state destruction; exception unwinding must also release scheduler leases
under the same synchronization. Modern standard libraries use
`atomic<shared_ptr>`; older Apple libraries use the atomic shared-pointer free
functions behind a feature test.

One snapshot is shared across clients. A slow client has one in-flight frame,
misses stale ticks and is disconnected on timeout. Browser history is capped.
This makes observability best-effort; metrics are neither a scheduling dependency
nor a recovery journal. The frontend validates external message structure before
rendering arrays or graph edges.

## Certification as evidence

A synthetic run is meaningful when its logical result is checked against an exact
reference, not merely when the process exits. The suite checks accepted/fetched
sets, cap and budget invariants, pressure transitions, retry recovery, durable
prefixes, mutation rollback and cross-engine resume. Fault injection is explicit
and test-only; a scoped VFS produces actual SQLite FULL/write errors without
filling a real disk. URL/robots/checkpoint fuzzers exercise parsers, not trusted
serializers in isolation.

The bounded-graph soak repeatedly destroys complete engine/server lifecycles in
one process. This separates retained-resource growth from expected seen-set
expansion. It is paced so that ephemeral-port churn does not dominate a memory
and teardown experiment. High-fanout load is certified separately. Compilers,
sanitizers, observed failures, unsupported tools and unrun platform gates remain
visible in the report. No blanket race suppressions or fabricated PASS results
are used.
