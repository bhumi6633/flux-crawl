# FluxCrawl optimization experiment

## Assessment recorded before implementation

Baseline revision: `01c7048` (completed Step 9). Experiment date: 2026-10-24,
America/Toronto. Same Apple M3/8 GiB/macOS 15.0.1 environment as the
[profiling report](../profiling-report.md); Release C++20/Apple clang 16.

Measured problem: shared `HtmlLinkExtractor` dominates link-heavy processing.
Step 9 async/16 spent a median 209.452 ms in extraction during 218.051 ms
elapsed; the median per-trial ratio was 96.1%. Five-second external sampling
without internal profiling put 4,138 of 4,304 main-thread observations in
response/extraction processing. Large subtrees included URL resolution, libxml
parsing and document cleanup. This affects all three engines and blocks the
single async event loop during response processing.

Hypothesis selected for one experiment: avoid resolving the same decoded,
whitespace-trimmed href repeatedly within one document and fixed base URL.
The link-heavy fixture has 1,000 extra anchors but only 100 unique references
per document. Currently every duplicate goes through `Url::resolve` and usually
`Url::parse`/curl URL parsing before canonical per-document deduplication.
A local owning set of processed reference strings could skip that repeated work.
Canonical URL dedup must remain because different references can resolve to the
same URL. First-occurrence order, invalid reference handling, relative bases,
query semantics and engine equivalence must remain identical.

This is a speed-for-memory hypothesis, not yet an accepted optimization. Retain
only if repeated Release comparisons show a material improvement on link-heavy,
with acceptable behavior on network-bound and low-duplicate controls. Measure
P50/P95/P99 as well as throughput, extraction time, CPU and peak RSS. No shared
cache or new synchronization is proposed; all cache state would die with the
extractor call. Check unique-reference memory cost and sanitizer lifetime safety.

Rejected before implementation: lock/CV changes (measured lock wait negligible),
seen-set reservation/global hash replacement (global discovery small), handle or
transfer pooling and response-buffer reserves (no dominant cost established),
parser replacement, worker-model changes and connection policy changes. libxml
initializes once already, and discovery already publishes a batch under one
lock. Do not change these without new evidence.

## Baseline and protocol

Before implementation: inspect all engines, URL/parser, crawl state, receive
buffer/callback, synchronization and transfer ownership; run all current tests;
reproduce the Step 9 Release workloads. Preserve the original executable and
fresh CSVs under ignored `results/step10-*`. Never overwrite baseline files.

Planned primary comparisons: profiling disabled, five measured repetitions and
one warmup, sequential once per repetition, threaded and async limits
1,2,4,8,16. Network uses 40 pages, fanout 39, 50 ms delay, 4 KiB bodies.
Link-heavy uses its existing 100-page/99-fanout/0-ms/64-KiB/1,000-shared-anchor
preset. A high-fanout control (100 pages/99 fanout/0 ms/4 KiB, no extra shared
anchors) checks overhead when the repeated-reference optimization has little
opportunity. Separate link-heavy profile sweeps use three repetitions and one
warmup. Every sample retains the runner's full logical-workload validation.
