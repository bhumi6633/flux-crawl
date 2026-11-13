# FluxCrawl interview and design guide

Use this as an explanation aid, not a script to memorize. Claims refer to the
current source and recorded experiments. Start with the [one-pager](project-one-pager.md)
and [URL walkthrough](design-walkthrough.md); follow evidence links when challenged.

## 30-second explanation

FluxCrawl is a C++20 networking project that runs the same crawling workload three
ways: sequential blocking I/O, a fixed blocking thread pool, and one event-driven
libcurl multi loop. Shared semantics make the comparison meaningful. I built local
workloads to measure throughput and latency, then addressed scheduling, retries,
bounded admission, durable recovery and exact disk-backed deduplication. A live
systems dashboard exposes that behavior. Crawling is the workload; concurrency
and systems tradeoffs are the main subject.

## 2-minute explanation

I wanted to compare networking architectures without changing the application
semantics every time. FluxCrawl therefore shares URL normalization, HTML parsing,
identity, depth, policy and scheduling rules across three engines. The sequential
engine is the baseline. Fixed workers overlap blocking requests and parsing,
coordinating shared state through a mutex and condition variable. The event-driven
engine owns crawl state on one thread and drives multiple libcurl transfers,
processing completions and refilling slots as they finish.

One hard problem was defining completion. An empty queue is temporary if another
worker can discover links, or if a retry is waiting for its deadline. A second was
backpressure: parking every worker as a producer can stop consumption entirely.
Instead, discoveries can be spooled to disk and admitted under high/low bounds,
with active-resource reservations preserving retry capacity.

Recovery uses logical state rather than sockets. Snapshots rewrite state;
SQLite batches coupled seen/work changes. Requests active at a durable boundary
may repeat, so the guarantee is at least once. Bloom is only a negative prefilter
in front of exact membership; false positives add database checks rather than
lose URLs.

On the local 50 ms profile, throughput rose from about 18 attempts/s sequential
to 216 threaded and 218 async at limit 16. On repeated-link processing, threads
reached about 2,619 while async stayed near 818 because parsing occupies its one
crawl thread. That contrast is the point: architecture choice depends on the
workload. Deterministic tests, fuzzing, storage/HTTP faults and a ten-minute
lifecycle soak support the implementation. Remote CI, Linux leak detection and
browser visual QA remain explicitly unexecuted release gates.

## Architecture and boundaries

### 1. Why three engines, and why crawling?

Crawling combines network waiting, response processing, dynamically generated
work and duplicate identity. Holding those semantics consistent lets the engine
loop become the main experimental variable. Sequential supplies a reference;
threads and multi demonstrate different ways to overlap I/O, not three unrelated
applications. See [engine comparison](../README.md#execution-engines).

### 2. Which components are actually shared?

`Url`, `HtmlLinkExtractor`, `CrawlState`, `CrawlPolicy`, `HostScheduler`, retry
classification and completion semantics are common. Engine-specific ownership
and waiting differ. Policies are shared implementations, while publication still
occurs under each engine's synchronization model; there is not one global
runtime scheduler used simultaneously by all engines.

### 3. Why libcurl multi rather than a socket stack or Boost.Asio crawler?

The current experiment compares libcurl easy and multi over common transport
behavior instead of adding a second HTTP/TLS implementation. Multi supplies
concurrent transfers and completion/poll APIs without request workers in FluxCrawl.
Boost.Asio/Beast is used for the telemetry server, not the crawl transport.
This is an explanation of the implemented boundary, not a claim that Asio would
be slower; that alternative was not benchmarked.

### 4. What does single-threaded async mean?

The FluxCrawl crawl loop mutates state and processes completions on one thread.
Multiple libcurl transfers can be in progress, but HTML work is serialized there.
The server and blocking-engine samplers have separate threads; libcurl's platform
internals are not promised to use zero threads. There are no coroutine/async-await
APIs or request-worker threads in this engine.

### 5. What does global deduplication mean?

It means one accepted canonical identity per crawl, not a single HTTP request for
all time. Fragments disappear, default ports normalize and queries remain part of
identity. Admission marks seen before fetching; a deferred candidate is not yet
seen. Retries and crash recovery can repeat attempts on the same accepted URL.

## Concurrency and lifecycle

### 6. How do you know the threaded crawler is finished?

An empty ready queue is insufficient while workers hold active leases or future
work has a deadline. The coordinator checks scheduler/deferred state and active
claims, then waits for publication or eligibility rather than exiting early.
Budget, graceful stop and fatal failure are explicit alternate endings. See
[coordinator](../src/thread_pool_crawler.cpp) and scheduling/retry integration tests.

### 7. Where are locks held, and how do duplicates race safely?

Workers acquire shared coordination for claims and publication, but perform
blocking HTTP and HTML processing outside that lock. Exact seen check/insertion
and transfer into accepted work happen under coordination, preventing two workers
from independently accepting the same canonical URL. Disk publication and durable
work still have costs; the design does not promise a lock-free or zero-I/O
critical section.

### 8. Why condition variables, and where did contention matter?

A condition variable releases the coordinator mutex while a worker waits, and its
predicate is checked again after wakeup, including spurious wakeups. Eligibility
deadlines handle retry/rate waits without a spin loop. The historical profiling
experiment found extraction dominant and lock wait negligible for its workload;
that evidence did not justify speculative lock redesign. It does not prove
contention is negligible for every workload; see [profiling](profiling.md).

### 9. How do callback pointers survive container growth and cleanup?

The async map holds `unique_ptr` transfer objects keyed by easy handle, so map
rehashing does not relocate the pointed-to request/buffer state. Handles are
removed from multi before owned callback state is destroyed. Completion result
data is copied before removal because curl message storage can become invalid.
These lifetime rules are visible in [async transfer ownership](../src/async_crawler.cpp).

### 10. How does the event loop avoid idle gaps and busy waiting?

It calls perform, drains available completion messages, publishes results and
refills eligible slots before polling again. It does not wait for a whole request
batch, although the shared BFS depth barrier can intentionally delay deeper work.
With no active transfers but future work, it sleeps until eligibility; with active
transfers, multi polling is constrained by scheduler and sampling deadlines.
The delayed-only test exercises 20 wakeups and checks a broad CPU bound.

## Scheduling, policy and memory

### 11. How do rate and concurrency limits differ?

An origin concurrency cap limits simultaneous scheduler claims; a token bucket
limits start rate with a configured burst. Fast responses can exceed a desired
request rate even under a small concurrency cap, while a rate limit alone can
permit many slow requests to overlap. Keys are scheme/host/effective port, not
DNS/IP identity. Robots and retries acquire the same scheduling constraints.

### 12. How do robots and retries interact with scheduling?

Policy decides whether ordinary resources are permitted; scheduling decides when
eligible work may start. One origin bootstrap holds ordinary resources until
policy resolves; exhausted/unavailable bootstrap outcomes deny that origin.
Retryable outcomes use shared backoff/jitter and Retry-After guidance, retaining
identity and depth. The robots matcher is a literal-prefix subset; curl redirect
hops do not get separate policy/scheduler leases.

### 13. Why not block producers when pending is full?

If all fixed workers finish pages and wait to publish, no worker may remain to
drain pending work. FluxCrawl instead streams discovered candidates to a disk
spool and lets producers return to consumption. High/low admission reservations
include active resources so retries can reenter without overshoot. Async uses
the same deferred-admission principle; it still pays synchronous parsing/disk cost.

### 14. What is still unbounded?

Admission bounds accepted unfinished resources, not every allocation or byte.
Retained attempts, host state, MemoryExact seen identities and spool/database
storage can grow; bodies and active slots have separate limits. Deferred counts
are candidate records and can include duplicates. It would be inaccurate to
claim constant-memory crawling from bounded pending queues alone.

### 15. Can Bloom false positives skip URLs?

No: Bloom is a negative prefilter, and maybe hits consult exact cache/SQLite state.
With `m` bits, `k` probes and `n` inserts, approximate false-positive probability
is `(1-exp(-k*n/m))^k`; saturation makes more lookups fall through to SQLite.
At 20× design capacity the measured 100k-query mix made every query consult exact
state while still returning correct answers. A Bloom-only visited set could drop
URLs, and is deliberately absent. See [deduplication](deduplication.md).

## Durability and failure

### 16. What happens if the process dies mid-request?

Active transport state is lost. Logical unfinished work from the durable boundary
is requeued, so a server may receive the same HTTP request again. URL identity
remains exact, and cumulative budget/retry metadata constrains replay. This is
at-least-once recovery, not exactly-once execution or an HTTP side-effect guarantee.

### 17. What is persisted, and why can another engine resume it?

Durable state represents identity, unfinished/terminal work, policy/retry state,
depth, counters and deferred discovery, not worker stacks or socket handles.
Restore validates compatibility and logical consistency before issuing requests.
Another execution engine can consume those same semantics; tests cover
Sequential→Async, Threaded→Async and Async→Threaded paths. Snapshot companions
are required, rather than optional files to discard.

### 18. Why provide snapshots and SQLite?

Snapshots are understandable full-state rewrites with temp validation/replacement;
SQLite provides incremental transactions and exact disk-backed membership.
They have different I/O, startup and operational costs, so neither is advertised
as universally superior. SQLite is single-owner in this application. Snapshot
replacement does not claim identical power-loss durability on every filesystem.

### 19. Why batch commits, and what if one fails?

Batching amortizes transaction cost but enlarges the uncommitted suffix that may
be replayed. Seen/work/terminal transitions must commit together so a durable
identity cannot lose its corresponding work. A failed SQLite transaction retains
the previous committed prefix and surfaces persistence failure. Store-level and
six engine/error tests inject actual FULL/write-I/O outcomes and verify recovery.

### 20. How do you distinguish retryable failure from permanent rejection?

The shared policy retries 429/500/502/503/504 and selected transport errors within
an explicit allowance. TLS validation, body limits and redirect loops do not
retry; parsing/content-type filtering do not manufacture retries. Backoff is
capped and may be jittered; Retry-After can extend eligibility. Retries consume
resource attempts, while robots infrastructure is separately counted; see [rules](retries.md).

## Measurement and optimization

### 21. What result best illustrates the architecture tradeoff?

On the final local 50 ms profile, sequential reached 18.1 attempts/s versus
216.5 threaded/16 and 218.3 async/16. On repeated-link processing, threaded/16
reached 2619.4 versus 818.2 async/16, whose serialized completion work plateaued
near four slots. These are medians on Apple M3, not a general internet throughput
claim. The [report](benchmark-report.md) includes variation and exact workloads.

### 22. How did you identify and change a bottleneck?

Historical internal profiling and external stack sampling both pointed to HTML
extraction/URL resolution, not coordinator locks. An owning per-document set
skipped repeated decoded references before resolution while preserving canonical
dedup and order. Matched async/16 link-heavy throughput changed from 499.92 to
820.06 attempts/s; reversed-order controls supported the finding. This applies
to the measured vector extractor, not a universal current bounded-path gain.

### 23. What optimization failed or was rejected?

Only one production optimization experiment was implemented in that phase and it
was retained; there was no implemented-and-reverted optimization to invent.
Lock/CV redesign, pooling and parser replacement were rejected before implementation
because the evidence did not justify them. An apparent low-duplicate speedup in
the initial order disappeared under reversed-order control. The honest lesson is
to reject the causal claim, not report that noise as another win.

### 24. What did feature measurements show?

Tighter admission bounds lowered peak unfinished work to the configured level
but added deferred I/O; the small RSS trial did not prove broad memory savings.
SQLite batching reduced transaction counts, without establishing one universal
optimal batch size. Correctly sized Bloom reduced exact checks from 100k to
5,906 on the 95%-absent mix; saturation removed the benefit. Dashboard 500 ms
runs overlapped baseline variation, so no precise overhead percentage is claimed.

### 25. How are benchmark comparisons kept defensible?

Fixtures fix graph, delays, bodies and duplicates; each run validates logical
counts/sets rather than just process exit. Final architecture data use Release,
excluded warmups and three measured runs, separately from sanitizer checks.
Profiles and advanced process timings have different timing bases and are not
mixed. Connection churn, server cost and run-order noise remain limitations.

## Testing, observability and defense

### 26. What did sanitizers and fuzzing establish?

Recorded local ASan/UBSan and TSan suites passed with no findings; that is evidence,
not proof of no possible race or leak. Four bounded libFuzzer targets cover URL,
resolution, robots and checkpoint parsing with invariants and corruption seeds.
Apple LeakSanitizer is unsupported locally, and Linux leak CI is not yet executed.
The [certification report](certification.md) distinguishes each result and rerun.

### 27. How did you test faults and crash recovery?

Deterministic HTTP fixtures produce truncated bodies, disconnects, timeouts,
retryable responses, redirect loops and oversized input. Test-only hooks exercise
checkpoint pre-write/replacement failure; a scoped SQLite VFS injects real FULL
and write-I/O errors. Process-kill, committed-prefix, active-reissue and cross-engine
resume tests check recovery semantics. The ten-minute same-process soak separates
teardown retention from expected growth in a large crawl.

### 28. Why immutable telemetry and what about slow clients?

Only bounded snapshots cross from crawl ownership into the server; the server
cannot inspect live scheduler containers. JSON and network writes therefore do
not hold the scheduler lock. Each client has one in-flight frame, misses stale
ticks and disconnects on a bounded write deadline; browser history is capped too.
Telemetry is sampled best-effort visibility, not the persistence journal.

### 29. What is a real bug and lesson from development?

The hardening audit fixed sequential exception unwinding that could release a
scheduler lease without first reacquiring the mutex used by telemetry sampling.
The fix restores the ownership rule even on failure paths; do not claim TSan
originally discovered it, because that is not recorded. The same audit corrected
IPv6 telemetry that double-bracketed an already bracketed host. These are concrete
examples of cleanup synchronization and representation boundaries, not invented incidents.

### 30. What would you change next, and is it release-ready?

Complete remote compiler/sanitizer execution, Linux leak detection and browser
visual QA before tagging the prepared 0.1.0 candidate. Optional future experiments
include DNS/IP-aware politeness, HTTP caching or separating CPU-heavy completion
work, each requiring new measurements. The project is suitable to discuss as a
completed systems experiment with explicit limits; it is not described as a
production-proven distributed service.

## Demo talking points

Follow [the exact demo workflow](demo.md). Explain these observations in order:

1. Show engine/configuration and active transfers: eight slots, cap two/origin.
   Multiple origins make concurrency distinct from one-host pressure.
2. Show retry state and recovered counts. A retry increases attempts, not accepted
   identity; the fixture deliberately fails selected resources once.
3. Show pending/admitted and deferred work. High=60/low=15 creates pressure;
   explain reservations and disk-backed admission rather than claiming all memory is bounded.
4. Show robots blocks and terminal progress: blocked logical URLs were never fetched.
5. Show Bloom occupancy/exact checks and SQLite commit metrics. This demo does
   not compare backends; use measured tables for performance claims.
6. Show sampled origin topology and final state during linger. It is a capped
   discovery view, not a full web graph. No browser screenshot/visual certification
   is claimed by the existing automated checks.

## Claim defense checklist

For any resume bullet, locate its code path, named test/evidence, ownership rule,
tradeoff and failure mode. The [resume claim map](resume-bullets.md#claim-evidence-map)
provides a starting point. If a number is questioned, name the profile, limit,
machine and repetition protocol; do not replace a scoped result with a universal claim.
