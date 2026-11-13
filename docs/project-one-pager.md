# FluxCrawl — technical one-pager

FluxCrawl uses crawling to investigate a concrete systems question: how much of
network throughput comes from overlapping I/O, and when does local processing
become the limiting resource? Its three C++20 engines execute shared URL,
HTML, depth, scheduling, retry and identity semantics through different loops.
The comparison is useful because policy is held consistent rather than rebuilt
for each architecture.

## Three implementations of one workload

The sequential engine is the reference: one blocking request, process, repeat.
The fixed thread pool performs blocking requests and parsing outside a shared
coordinator lock, then publishes state under that lock. Workers use condition
variables and eligibility deadlines. The async engine drives libcurl multi on
one crawl thread, drains completions and immediately refills eligible slots;
request I/O overlaps, while HTML processing remains serialized. Separate local
telemetry I/O does not change those request execution models.

## Five hard engineering problems

1. **Knowing when to finish.** An empty ready queue can coexist with active
   workers, delayed retries or rate-limited work. Completion considers all those
   states; budgets and stops have separate outcomes.
2. **Keeping the comparison fair.** Shared canonical identity, shortest-depth
   barriers, retry budgets and origin scheduling prevent execution speed from
   silently changing the logical workload.
3. **Handling discovery faster than consumption.** High/low reservations include
   active resources so retries retain capacity. Disk-backed deferred admission
   preserves candidates without parking all workers as producers.
4. **Recovering a consistent prefix.** SQLite commits seen and work transitions
   together; snapshots retain companion discovery data. Active HTTP work may be
   repeated after recovery, so the promise is at least once.
5. **Making an optimization safe for correctness.** Bloom only avoids definite
   negative database queries. Maybe hits always receive exact verification;
   saturation harms performance without dropping URLs.

## What was measured

Four deterministic loopback profiles cover latency, repeated-link processing,
fanout and mixed latency. The final architecture suite contains 84 measured
Release samples plus excluded warmups. Separate experiments measure pressure,
SQLite batch thresholds, 100k-identity exact/Bloom lookups, restoration and
telemetry. Apple M3 / Apple Clang 16 results are workload-scoped:

| Attempts/s, median | Sequential | Threaded/16 | Async/16 |
| --- | ---: | ---: | ---: |
| 50 ms latency-bound | 18.1 | 216.5 | 218.3 |
| Processing-heavy | 704.9 | 2619.4 | 818.2 |

Both concurrent engines hide latency. Threaded parsing wins on the processing
profile; async plateaus around four slots. That is a measured architectural
limit, not a reason to declare either model universally superior.

A prior matched optimization targeted repeated URL resolution inside the shared
vector extractor. Avoiding repeat decoded references improved async/16 from
499.92 to 820.06 attempts/s on that specific link-heavy experiment; reversed-order
controls supported the gain. Low-duplicate controls did not demonstrate a gain.
The final default bounded path uses streaming extraction instead, so the old
experiment is not presented as a universal current-path speedup.

## Failure evidence and operational view

Local certification passed 76 normal tests, 78 Release/heavy tests, ASan/UBSan,
TSan ownership checks and four fuzz campaigns totaling over six million inputs.
Real SQLite FULL/write-I/O failures, checkpoint replacement failure, deterministic
HTTP chaos and cross-engine resume are tested. A ten-minute bounded-graph soak
completed 2,938 lifecycles with stable three descriptors after destruction.
The live React console reads bounded immutable snapshots through loopback
REST/WebSockets, including pressure, retries, host activity and storage metrics.

These are executed local results, not a universal safety proof. Remote CI,
Linux leak detection and real browser visual QA remain release-tag gates.
Version 0.1.0 is prepared; no green remote status is claimed.

## Lessons, scope and possible next experiments

Concurrency alone cannot remove serialized parsing cost. Bounded admission
moves pressure to disk rather than eliminating all growth. Durability needs
atomic logical transitions; telemetry needs its own backpressure. A single-process
crawler can still retain growing records, hosts and disk state. Robots uses a
literal-prefix subset and origin keys; redirect hops do not acquire separate
policy leases. There is no browser rendering or response cache.

The immediate next work is completing the outstanding validation gates. Optional
future experiments could measure DNS/IP-aware politeness, HTTP caching or moving
CPU-heavy completion work off the event loop. Those are new tradeoffs, not current
features or prerequisites for understanding the completed architecture experiment.

Read the [walkthrough](design-walkthrough.md), [benchmarks](benchmark-report.md),
[certification](certification.md) and [interview guide](interview-guide.md).
