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

Fresh baseline reproduced before source changes: all 46 tests pass. Link-heavy
median throughput is 440.73 requests/s sequential, 1,728.25 threaded/8 and
499.92 async/16. Network sequential is 18.12 requests/s. The original executable
is `results/step10-before-fluxcrawl`, SHA-256 `a75828b91cdc20e6d041bf8ba93c07ca024f36b3526492eab29211ab3cd0c5e8`.

## Experiment 1: exact-reference deduplication before URL resolution

**Problem → evidence:** repeated URL resolution is inside the measured shared
extractor hotspot. Fresh baseline reproduces the plateau; the Step 9 evidence
is not used as a mismatched “before” timing.

**Hypothesis → change:** `src/html_parser.cpp` now uses one local
`std::unordered_set<std::string>` to own processed decoded/trimmed references.
An exact repeat skips resolution. The existing `unordered_set<Url>` remains
responsible for canonical deduplication. This is four additional lines, including
comments; no shared cache, custom allocator, guessed reserve capacity or HTTP
policy change. It benefits all engines through the shared extractor.

**Correctness:** regression tests passed before and after the implementation.
They cover entity-decoded/trimmed duplicates mixed with canonical aliases,
first-occurrence order, repeated invalid references, 300 long unique hrefs
followed by duplicates (owning string lifetime/rehash), and reuse of the extractor
with different document bases. Existing depth/budget/failure/redirect/body-limit,
parser, URL and three-engine equivalence tests still pass. No lock, CV, scheduling,
callback storage, transfer lifetime or immediate-refill behavior changed.

**Benchmark → decision:** keep `66f94d3`. Primary repeated measurements show a
large link-heavy benefit; reversed-order confirmation supports it. Network and
low-duplicate results do not demonstrate a meaningful speedup. Exactly one
production experiment was implemented; none were reverted. Unjustified candidates
were rejected before implementation, rather than manufacturing extra experiments.

## Matched primary results

Each row is a median across **five** normal-mode trials, with one discarded
warmup. Latency columns are medians of each trial's P50/P95/P99, not pooled
percentiles. “CV” is population standard deviation / mean of elapsed time,
expressed as a percentage; it describes variation, not a confidence interval.
The baseline and candidate use identical workloads, limits, policy and Release
flags. No builds/tests ran during measurements. Temporary `caffeinate -i`
assertions were used; no settings persisted. None of these Step 10 sweeps failed
validation. Files are `results/step10-{before,after}-{scenario}-normal.csv`.

### latency-bound

| Engine/limit | Before req/s | After req/s | Change | P50 ms before/after | P95 ms before/after | P99 ms before/after | Elapsed CV % before/after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sequential/1 | 18.12 | 18.06 | -0.3% | 55.420 / 55.357 | 57.146 / 56.873 | 59.681 / 57.873 | 0.74 / 0.82 |
| threaded/1 | 18.14 | 18.05 | -0.5% | 54.966 / 54.985 | 57.171 / 57.230 | 57.655 / 57.635 | 0.95 / 0.55 |
| threaded/2 | 34.21 | 34.63 | +1.2% | 55.887 / 55.180 | 57.256 / 56.910 | 58.432 / 59.986 | 0.87 / 1.20 |
| threaded/4 | 65.74 | 66.27 | +0.8% | 55.154 / 54.890 | 57.179 / 57.395 | 57.993 / 58.610 | 1.70 / 0.95 |
| threaded/8 | 116.66 | 119.64 | +2.6% | 56.600 / 55.177 | 59.721 / 57.729 | 60.212 / 60.157 | 2.50 / 1.08 |
| threaded/16 | 171.27 | 174.68 | +2.0% | 58.396 / 57.400 | 61.951 / 61.342 | 63.221 / 61.591 | 2.36 / 3.38 |
| async/1 | 18.23 | 18.11 | -0.6% | 54.714 / 55.173 | 56.724 / 56.693 | 57.608 / 57.179 | 0.72 / 0.75 |
| async/2 | 35.19 | 34.53 | -1.9% | 54.210 / 55.495 | 55.965 / 57.287 | 56.359 / 60.888 | 0.86 / 1.10 |
| async/4 | 66.29 | 65.58 | -1.1% | 54.711 / 55.709 | 56.545 / 57.138 | 57.688 / 58.687 | 1.47 / 1.24 |
| async/8 | 119.65 | 117.79 | -1.6% | 54.813 / 56.352 | 56.781 / 59.277 | 57.396 / 59.885 | 1.62 / 1.20 |
| async/16 | 176.65 | 177.38 | +0.4% | 56.352 / 56.574 | 59.730 / 60.378 | 60.322 / 61.347 | 2.90 / 2.35 |

### link-heavy

| Engine/limit | Before req/s | After req/s | Change | P50 ms before/after | P95 ms before/after | P99 ms before/after | Elapsed CV % before/after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sequential/1 | 440.73 | 700.37 | +58.9% | 0.337 / 0.326 | 0.437 / 0.362 | 0.470 / 0.376 | 3.23 / 1.58 |
| threaded/1 | 441.11 | 703.62 | +59.5% | 0.333 / 0.323 | 0.523 / 0.366 | 0.599 / 0.390 | 6.33 / 1.05 |
| threaded/2 | 841.72 | 1316.19 | +56.4% | 0.352 / 0.335 | 0.425 / 0.380 | 0.461 / 0.428 | 4.60 / 0.29 |
| threaded/4 | 1594.88 | 2462.38 | +54.4% | 0.346 / 0.310 | 0.460 / 0.399 | 0.735 / 0.750 | 13.79 / 1.46 |
| threaded/8 | 1728.25 | 2695.87 | +56.0% | 1.285 / 1.496 | 2.452 / 1.836 | 2.857 / 2.212 | 8.60 / 0.55 |
| threaded/16 | 1670.48 | 2680.55 | +60.5% | 5.435 / 4.268 | 7.084 / 4.798 | 7.587 / 5.181 | 22.00 / 0.73 |
| async/1 | 451.12 | 708.57 | +57.1% | 0.311 / 0.296 | 0.372 / 0.331 | 0.403 / 0.347 | 3.89 / 0.56 |
| async/2 | 479.67 | 802.72 | +67.3% | 2.211 / 1.365 | 2.294 / 1.411 | 3.018 / 1.437 | 2.69 / 0.50 |
| async/4 | 494.09 | 827.95 | +67.6% | 5.732 / 3.559 | 6.271 / 3.737 | 6.449 / 3.826 | 5.29 / 1.38 |
| async/8 | 493.76 | 823.27 | +66.7% | 10.473 / 7.213 | 14.230 / 8.631 | 14.341 / 8.668 | 2.06 / 2.45 |
| async/16 | 499.92 | 820.06 | +64.0% | 18.478 / 13.916 | 30.119 / 18.326 | 30.219 / 18.411 | 3.82 / 1.55 |

### high-fanout

| Engine/limit | Before req/s | After req/s | Change | P50 ms before/after | P95 ms before/after | P99 ms before/after | Elapsed CV % before/after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| sequential/1 | 5526.94 | 6041.29 | +9.3% | 0.150 / 0.134 | 0.179 / 0.150 | 0.215 / 0.162 | 2.69 / 6.88 |
| threaded/1 | 5550.69 | 6122.11 | +10.3% | 0.149 / 0.133 | 0.165 / 0.148 | 0.198 / 0.155 | 5.81 / 1.24 |
| threaded/2 | 9151.68 | 10450.32 | +14.2% | 0.169 / 0.155 | 0.262 / 0.177 | 0.349 / 0.200 | 1.91 / 0.93 |
| threaded/4 | 8857.20 | 10664.63 | +20.4% | 0.383 / 0.324 | 0.584 / 0.415 | 0.731 / 0.512 | 13.07 / 1.19 |
| threaded/8 | 9273.82 | 10585.60 | +14.1% | 0.790 / 0.679 | 0.903 / 0.795 | 1.059 / 0.883 | 8.62 / 1.66 |
| threaded/16 | 8602.80 | 10438.23 | +21.3% | 1.642 / 1.396 | 2.104 / 1.593 | 2.192 / 1.710 | 13.29 / 33.61 |
| async/1 | 6424.96 | 7069.39 | +10.0% | 0.117 / 0.106 | 0.159 / 0.116 | 0.183 / 0.124 | 2.13 / 1.51 |
| async/2 | 10088.57 | 10956.20 | +8.6% | 0.149 / 0.131 | 0.256 / 0.248 | 0.326 / 0.279 | 5.35 / 3.35 |
| async/4 | 10457.02 | 11001.91 | +5.2% | 0.321 / 0.304 | 0.469 / 0.445 | 0.618 / 0.574 | 9.55 / 3.94 |
| async/8 | 10687.90 | 11207.10 | +4.9% | 0.653 / 0.642 | 0.908 / 0.765 | 0.991 / 0.839 | 8.56 / 1.07 |
| async/16 | 10265.49 | 11073.74 | +7.9% | 1.418 / 1.328 | 1.664 / 1.447 | 1.758 / 1.499 | 11.36 / 1.29 |

The primary link-heavy gain is about 54–68% across the full sweep. Async/16
P95 falls from 30.119 to 18.326 ms and P99 from 30.219 to 18.411 ms. Not every
latency statistic improves: threaded/8 P50 rises from 1.285 to 1.496 ms while
P95/P99 fall. Faster response processing can increase pressure on the shared
server; these elapsed request metrics include scheduling and, for async,
completion-observation delay. No universal latency improvement is claimed.

Network differences are small (roughly -1.9% to +2.6% throughput). For example,
async/16 changes +0.4% with elapsed CV around 2–3%, while its P99 rises about
1 ms. There is no demonstrated network improvement. The network root has only
39 siblings, so finite-work/startup effects remain important.

The initial high-fanout sequence appears faster after the change, even though
there is almost no duplicate-resolution opportunity. That is evidence of
run-order/machine effects, not a claimed optimization win. Its trials are only
about 9–18 ms long. The following confirmation controls for order more carefully.

## Reversed-order confirmation and noise

For both link-heavy and high-fanout, retain the same server for four fixed blocks:
**candidate, baseline, baseline, candidate**. Each block uses three measured
repetitions, one warmup, all engines, limits 1/8/16. Pool all six measured
observations per version/configuration without choosing favorable trials.
These are separate confirmation samples, not substituted into the primary table.

| Scenario | Engine/limit | Before req/s | After req/s | Change |
| --- | --- | ---: | ---: | ---: |
| link-heavy | sequential/1 | 410.54 | 663.68 | +61.7% |
| link-heavy | threaded/1 | 409.37 | 677.03 | +65.4% |
| link-heavy | threaded/8 | 1367.53 | 2295.50 | +67.9% |
| link-heavy | threaded/16 | 1338.45 | 2077.41 | +55.2% |
| link-heavy | async/1 | 393.19 | 659.14 | +67.6% |
| link-heavy | async/8 | 475.88 | 798.68 | +67.8% |
| link-heavy | async/16 | 464.98 | 796.44 | +71.3% |
| high-fanout | sequential/1 | 5949.79 | 5943.47 | -0.1% |
| high-fanout | threaded/1 | 6032.06 | 6004.65 | -0.5% |
| high-fanout | threaded/8 | 10564.14 | 10347.83 | -2.0% |
| high-fanout | threaded/16 | 10323.88 | 10242.87 | -0.8% |
| high-fanout | async/1 | 7113.11 | 7058.95 | -0.8% |
| high-fanout | async/8 | 11035.60 | 11032.50 | -0.0% |
| high-fanout | async/16 | 11271.40 | 11145.62 | -1.1% |

The link-heavy improvement remains 55–71% in confirmation. High-fanout is
-0.03% to -2.05%, with overlapping elapsed ranges in every configuration;
threaded/8 elapsed CV is 2.62% before and 11.31% after. This supports “no
meaningful demonstrated change” in that low-duplicate control, not a gain.
It does not prove zero overhead on all inputs. This control is sparse except
for the root; a large document containing entirely unique hrefs may incur
additional cache costs and is not characterized by these measurements.

## Internal attribution and external confirmation

Separate link-heavy profile sweeps have three measured repetitions and one
warmup. They are not used for headline throughput. Selected medians:

| Engine/limit | Elapsed ms before/after | Extraction ms before/after |
| --- | ---: | ---: |
| sequential/1 | 239.857 / 142.758 | 198.422 / 110.289 |
| threaded/8 | 70.417 / 37.474 | 373.550 / 129.509 |
| async/16 | 211.871 / 123.965 | 203.775 / 118.563 |

Threaded extraction is summed across workers; it can exceed wall time. Async
extraction still occupies about 96% of elapsed at sixteen after optimization.
The single-thread processing constraint remains, at a higher throughput level.
Async remains roughly flat from concurrency four onward. Threaded still reaches
its best primary median at eight; eight and sixteen are now close. These results
do not establish that the optimal worker count or concurrency plateau moved.

Both binaries were sampled separately with `/usr/bin/sample <pid> 5 1 -mayDie`
during 50 repetitions of normal-mode async/16 link-heavy. Symbolized main-thread
observations (inclusive function subtrees, overlapping categories):

| Stack category | Before observations | After observations | Share of main thread before/after |
| --- | ---: | ---: | ---: |
| Main thread total | 4,336 | 4,304 | 100% / 100% |
| HtmlLinkExtractor | 4,147 | 4,016 | 95.6% / 93.3% |
| Url::resolve | 1,890 | 310 | 43.6% / 7.2% |
| htmlDoRead | 1,575 | 2,491 | 36.3% / 57.9% |
| xmlFreeDoc | 400 | 705 | 9.2% / 16.4% |

Counts include complete subtrees across call sites, before the profiler's
summary/leaf lists; they are not exact CPU percentages. More after-sample
parsing observations do not imply slower parsing: each second completes more
pages and URL work consumes a smaller share. URL resolution moves below DOM
construction/cleanup in the observed ranking. Existing libxml parsing and cleanup
are the next measured costs. No custom parser or further optimization is added.

## CPU, memory and complexity tradeoff

Three additional paired `/usr/bin/time -l` runs per binary, alternating order,
each perform ten validated async/16 link-heavy repetitions without profiling.
Median whole-process results (server is a separate process):

| Metric | Before | After |
| --- | ---: | ---: |
| Real seconds | 2.11 | 1.27 |
| User CPU seconds | 2.02 | 1.22 |
| System CPU seconds | 0.03 | 0.03 |
| Maximum RSS, bytes | 17,956,864 | 14,450,688 |
| macOS peak physical footprint, bytes | 8,734,336 | 11,765,056 |

Maximum RSS decreases about 19.5%, but peak physical footprint increases about
34.7% (8.33 to 11.22 MiB). These OS accounting measures are different, and neither
isolates the href cache. Report both; do not claim a general memory reduction.
The first unpaired time probes are also retained, but not mixed into these paired
medians. CPU time corroborates reduced work independently of internal timers.

The explicit cost is an additional owning hash set proportional to the number
and lengths of unique decoded references per active document. It is released
at return and contains no cross-document or process-global entries. On unique-link
pages it adds hashing/storage without avoiding resolution; peak overhead across
multiple parsing workers is not measured here. On the measured workload the
roughly 3 MiB footprint increase and four-line implementation are an acceptable
tradeoff for the confirmed large gain. No custom memory management or dangling
views are introduced. The set owns strings because `xmlNodeGetContent` storage
is freed on each iteration. This is an input-dependent optimization, not a
universal improvement claim.

## Verification and reproduction

All 46 tests pass before and after. After the change, ASan/UBSan passes 46/46;
ThreadSanitizer passes `core.thread_pool_crawler`, `core.profiling` and
`benchmark.integration` (3/3). No sanitizer findings. No synchronization change
was made; existing stress cases and TSan exercise shared-parser use by workers.
No timing thresholds were added to CI. Preserve the existing link-heavy fixture
and correctness cases as the regression workload.

Commands executed for standard/Release verification:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
cmake --build build-sanitize
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure
cmake --build build-tsan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan \
  -R 'core.thread_pool_crawler|core.profiling|benchmark.integration' --output-on-failure
```

Sanitizer build flags are recorded in the [Step 9 report](../profiling-report.md#10-verification).
No sanitizer timings enter performance tables. To independently reconstruct the
baseline, build revision `01c7048` in a separate checkout with the same Release
configuration; the retained binary is a convenience, not a required dependency.

Start one server at a time with the parameters listed in the protocol above.
For example, processing:

```sh
python3 tools/benchmark_server.py --port 8080 --scenario link-heavy
```

Run each executable separately with fresh output filenames:

```sh
results/step10-before-fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario link-heavy --engine all --concurrency 1,2,4,8,16 \
  --runs 5 --warmups 1 --output results/reproduce-before.csv
build-release/fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario link-heavy --engine all --concurrency 1,2,4,8,16 \
  --runs 5 --warmups 1 --output results/reproduce-after.csv
```

Repeat with the network and high-fanout server overrides from the protocol.
Profile sweeps add `--profile` and use `--runs 3`. Confirmation uses
`--concurrency 1,8,16 --runs 3 --warmups 1`, in candidate/baseline/baseline/candidate
order against the same running server. External sample/time commands follow
[the profiling guide](profiling.md#external-tools), using each binary and
async/16; the time probes use ten runs and no warmup. Prefix long commands with
`/usr/bin/caffeinate -i` on macOS if desired. CSVs are never overwritten.

Raw evidence stays ignored in `results/step10-*`: primary CSVs/console output,
confirmation CSVs, before/after samples, and repeated resource captures.
Only this curated report, tests, shared extractor change and README link are
committed for Step 10. The highest-value next experiment is attributing DOM
construction/cleanup allocation cost across realistic link densities, with a
unique-reference stress control and server CPU measured separately. Stop here
rather than combining another speculative change with this accepted result.

Candidate executable SHA-256: `0b1dd04110daa14e161cfa7f643b2069286f9f60bba1b8f5a07fba2c4bd66060`.
