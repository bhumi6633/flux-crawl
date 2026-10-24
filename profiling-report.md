# FluxCrawl initial profiling report

## 1. Experimental setup

Measured on **2026-10-23 (America/Toronto)**, Apple M3 (8 logical CPUs), 8 GiB RAM,
macOS 15.0.1 (24A348), arm64. Apple clang 16.0.0, C++20, CMake Release,
SDK/system libcurl and libxml2, Python 3.12.4 benchmark server.
Implementation revision: `cb20470` (instrumentation begins at `a45030f`).
No speculative production optimization was applied.

Client and concurrent Python HTTP/1.0 server ran on the same machine over IPv4
loopback. No builds or correctness/sanitizer tests ran during performance
sweeps. The primary matrix ran under `/usr/bin/caffeinate -i` to prevent idle
system sleep for the lifetime of the experiment process. Other desktop activity, CPU scheduling, power/thermal conditions,
connection establishment and server overhead remain uncontrolled confounds.
These results describe this workload and machine, not universal architecture
rankings or Internet crawling performance.

Every configuration had one validated, discarded warmup and three validated
measured trials. The runner used repetition-major ordering. Normal sweeps ran
before profiled sweeps. Tables use medians consistently; no fastest-trial
selection. Profile category medians are computed independently and should not
be added together. Raw CSV/text captures remain untracked in `results/`.

Before changes, all 45 tests passed. A small Release baseline (20 pages,
fanout 4, 20 ms delay, 1 KiB responses, two trials, no warmup) measured:

| Configuration | Median elapsed (ms) | Median requests/s |
| --- | ---: | ---: |
| Sequential | 501.911 | 39.850 |
| Threaded 1 | 506.839 | 39.460 |
| Threaded 4 | 155.785 | 128.388 |
| Async 1 | 503.466 | 39.727 |
| Async 4 | 155.856 | 128.326 |

This baseline verifies operation before instrumentation. It uses a different
workload from the main experiment and is not a before/after speed comparison.

## 2. Scenarios and matrix

| Scenario | Pages | Fanout | Delay | Body | Extra shared anchors/page | Threaded limits | Async limits |
| --- | ---: | ---: | --- | ---: | ---: | --- | --- |
| Network: latency-bound with overrides | 40 | 39 | 50 ms | 4 KiB | 0 | 1,4,8,16,32 | 1,4,8,16,32 |
| Processing: link-heavy defaults | 100 | 99 | 0 ms | 64 KiB | 1,000 | 1,2,4,8,16 | 1,2,4,8,16 |

Both include sequential at one. Depth limit 20, page budget 100, same-host,
proxy bypass, no redirects. The roots expose 39 and 99 siblings respectively, enough ready work for
the selected limits; the initial root remains serial. Link-heavy has 100,099
anchors in total, with 100,000 references to shared first-level/root URLs.
Per-document extraction deduplicates these to 100 unique targets per page;
global discovery considers 10,000 links but accepts only 99 beyond the seed.
All reachable pages are parsed. Exact body-byte totals are 163,840 and 6,553,600.

The mixed-latency fixture (5/20/50/100 ms by page ID modulo four) was exercised
by correctness tests, not included in the headline scaling sweeps. The network
sweep was reduced to 40 pages after host interruptions. This limits
high-concurrency inference: there are only two waves at concurrency 32,
plus the serial root. The processing scenario retains 100 pages.

Reproduction commands and measurement boundaries are in
[docs/profiling.md](docs/profiling.md). Main raw filenames are
`results/step9-final-{latency-bound,link-heavy}-{normal,profile}.csv`.

An earlier attempt was rejected by logical-workload validation before useful
error details were available. A subsequent attempt completed a normal network
sweep but rejected a profiled async/1 trial with 99 successes and one error:
"Operation timed out after 186154 milliseconds with 0 bytes received".
The configured request timeout was 30 seconds. This is consistent with a host
pause or severe scheduling interruption, but its cause was not independently
established. Validation discarded that entire profiled report. Failure diagnostics
now identify the configuration, counts and first HTTP error. The earlier normal
sweep is retained only as a raw pilot; the tables below use the complete matrix
rerun under the idle-sleep assertion, with the smaller network matrix above and all
repetitions included. A 100-page rerun under the assertion also failed during
async/1 warmup with a 931,905 ms timeout; the assertion did not prevent that host
interruption. The final shorter matrix completed all 132 measured and 44 warmup
crawls without validation failures. No retry policy was added to the crawler.

To reproduce the final network matrix, use the documented commands with server
`--pages 40 --fanout 39 --body-bytes 4096` and both client lists
`1,4,8,16,32`. Processing uses the documented default `link-heavy` server and
both lists `1,2,4,8,16`. Runs=3 and warmups=1 in both modes. External profiling
ran separately after the sweeps, without internal profiling.

## 3. Sequential baseline and primary throughput

These tables are **profiling disabled**. P95 is the median of the three
per-trial P95s, not a pooled percentile. Speedup uses the same engine's
concurrency-one median throughput. Short processing trials and only three
samples limit precision; small ordering differences are not robust rankings.

### latency-bound

| Engine | Limit | Median ms | Requests/s | P95 ms | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| sequential | 1 | 2212.255 | 18.081 | 56.788 | 1.00x |
| threaded | 1 | 2216.296 | 18.048 | 56.868 | 1.00x |
| threaded | 4 | 608.116 | 65.777 | 56.841 | 3.64x |
| threaded | 8 | 334.569 | 119.557 | 57.399 | 6.62x |
| threaded | 16 | 227.352 | 175.939 | 59.205 | 9.75x |
| threaded | 32 | 180.956 | 221.049 | 66.278 | 12.25x |
| async | 1 | 2199.541 | 18.186 | 57.217 | 1.00x |
| async | 4 | 597.618 | 66.932 | 56.006 | 3.68x |
| async | 8 | 335.206 | 119.330 | 58.262 | 6.56x |
| async | 16 | 222.484 | 179.788 | 57.108 | 9.89x |
| async | 32 | 172.403 | 232.014 | 61.519 | 12.76x |

### link-heavy

| Engine | Limit | Median ms | Requests/s | P95 ms | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| sequential | 1 | 277.721 | 360.074 | 0.798 | 1.00x |
| threaded | 1 | 263.030 | 380.184 | 0.717 | 1.00x |
| threaded | 2 | 120.999 | 826.454 | 0.419 | 2.17x |
| threaded | 4 | 72.755 | 1374.482 | 1.056 | 3.62x |
| threaded | 8 | 66.092 | 1513.040 | 2.192 | 3.98x |
| threaded | 16 | 72.047 | 1387.978 | 8.984 | 3.65x |
| async | 1 | 246.801 | 405.184 | 0.505 | 1.00x |
| async | 2 | 210.237 | 475.654 | 2.483 | 1.17x |
| async | 4 | 220.029 | 454.485 | 6.935 | 1.12x |
| async | 8 | 213.682 | 467.986 | 15.115 | 1.15x |
| async | 16 | 206.885 | 483.360 | 31.315 | 1.19x |

At concurrency one, network elapsed times differ by less than 1%; no meaningful
framework-overhead ranking follows from that difference. Processing medians are
277.721/263.030/246.801 ms for sequential/threaded/async at one, but the separate
profiled sequence changes those relative timings. More interleaved trials are
needed to isolate framework overhead from machine variability.

## 4. Threaded scaling and synchronization

The following are **profiling enabled** medians. Extraction/idle/lock times are
summed across workers, not CPU times and not disjoint portions of crawl elapsed.

| Scenario | Workers | Elapsed ms | Extraction ms | Lock wait ms | Idle ms | Max active |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| latency-bound | 1 | 2210.269 | 6.331 | 0.017 | 0.000 | 1 |
| latency-bound | 4 | 601.742 | 6.199 | 0.030 | 222.829 | 4 |
| latency-bound | 8 | 346.135 | 5.225 | 0.144 | 461.968 | 8 |
| latency-bound | 16 | 224.333 | 4.338 | 0.029 | 1333.632 | 16 |
| latency-bound | 32 | 173.038 | 4.323 | 0.080 | 3203.166 | 32 |
| link-heavy | 1 | 257.675 | 204.790 | 0.012 | 0.000 | 1 |
| link-heavy | 2 | 135.435 | 216.336 | 0.007 | 2.484 | 2 |
| link-heavy | 4 | 76.469 | 248.861 | 0.015 | 7.535 | 4 |
| link-heavy | 8 | 61.093 | 358.491 | 0.118 | 17.906 | 8 |
| link-heavy | 16 | 64.624 | 405.954 | 0.267 | 39.347 | 16 |

Network throughput improves through 32 workers. Aggregate idle grows with worker
count because the initial root cannot feed all workers and the small graph has
an underfilled tail. The network/32 median idle sum is 3,203 ms, while the crawl
lasts 173 ms; this is expected for sums over 32 workers. The measured lock wait
is only 0.080 ms in that configuration. Lock contention is not supported as the
cause of diminishing throughput gains here.

Processing throughput peaks at eight workers in both normal and profiled
sequences, then falls at sixteen. Normal throughput drops about 8.3% from
1,513 to 1,388 requests/s. The profiled lock wait rises from 0.118 to 0.267 ms,
remaining small relative to worker activity (about 458 and 930 ms respectively).
The extraction sum rises too, but that cannot by itself distinguish CPU resource
competition, allocator behavior, scheduling, or server limits. This is a measured
plateau/decline in this range, not proof that the crawler mutex caused it.

## 5. Async scaling and event-loop behavior

**Profiling enabled** medians; completion processing includes extraction and
discovery. Refill time is separate and below 0.6 ms in every configuration's
median. Every network trial has 40 completions/refills; every processing trial
has 100. Max active transfers reaches each configured limit.

| Scenario | Limit | Elapsed ms | Extraction ms | Poll wait ms | Completion ms | Iterations | Max active |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| latency-bound | 1 | 2196.168 | 5.807 | 2172.847 | 7.701 | 122 | 1 |
| latency-bound | 4 | 614.924 | 6.586 | 586.680 | 8.179 | 91 | 4 |
| latency-bound | 8 | 339.225 | 5.811 | 315.948 | 7.112 | 67 | 8 |
| latency-bound | 16 | 227.819 | 4.433 | 212.681 | 5.514 | 44 | 16 |
| latency-bound | 32 | 171.746 | 2.161 | 162.682 | 2.487 | 20 | 32 |
| link-heavy | 1 | 236.109 | 197.599 | 32.401 | 198.524 | 314 | 1 |
| link-heavy | 2 | 257.789 | 226.103 | 14.351 | 227.665 | 223 | 2 |
| link-heavy | 4 | 255.537 | 238.687 | 4.865 | 241.398 | 138 | 4 |
| link-heavy | 8 | 220.831 | 212.867 | 1.014 | 214.177 | 62 | 8 |
| link-heavy | 16 | 218.051 | 209.452 | 1.154 | 211.282 | 33 | 16 |

In the network workload, poll waiting dominates: median per-trial poll/elapsed
ratio is 98.9% at one and 92.3% at sixteen. The normal equal-limit comparisons
at 4/8/16 are 65.8/119.6/175.9 requests/s for threaded versus
66.9/119.3/179.8 for async. These models are close in this controlled I/O case;
a worker and an attached transfer are still different units of concurrency.

In link-heavy, normal async throughput varies only from 454 to 483 requests/s
at limits 4 through 16. At sixteen, extraction consumes a median 96.1% of crawl
elapsed and polling only 0.41% (medians of within-trial ratios). Processing on
the single event-loop thread is a supported bottleneck. Higher attached-transfer
limits do not parallelize the extractor. Observed request P95 grows from
0.505 ms at one to 31.315 ms at sixteen in normal mode, consistent with
completion observation being delayed by synchronous processing; this is not
proof of a corresponding increase in server/network service time.

## 6. Common metrics, server evidence and perturbation

Sequential network extraction is 6.710 ms in 2,224.819 ms profiled elapsed
(0.30% median within-trial ratio); controlled request delay/lifetime dominates.
Sequential link-heavy extraction is 186.460 ms in 223.418 ms (83.5%). The
extractor category includes normalization and per-page dedup, not just DOM parse.
Global discovery is much smaller: in link-heavy its median stays below 0.42 ms
for every configuration, despite 10,000 considered links and 99 acceptances.
There is no evidence here to prioritize optimizing the global frontier/set lock.

The network server's peak active count reaches every requested limit, including
32. On link-heavy it is usually only one or two, even while client active work
reaches its cap. With zero artificial delay, a handler can finish body generation
before another starts; the counter excludes response transmission. Low server
peak alone cannot establish saturation or underutilization. Python body creation
and client processing share this eight-core machine; server CPU profiling or a
separate host is required to resolve that confound at high threaded concurrency.

Profile mode is not a clean overhead experiment. For example, sequential
link-heavy changes from 360 to 448 requests/s between normal and profiled
sequences; this does not mean timers improve performance. The runs were ordered,
short, and machine conditions varied. Normal results remain the headline, and
profile results support category attribution only. Timing categories overlap;
no sum-to-wall or invented residual “network wait” calculation is used.

## 7. Bottlenecks supported by measurements

- Network workload: request delay/lifetime and async readiness waits dominate;
  parsing and measured coordination are small. Finite work and serial startup
  constrain efficiency. The tested range does not establish an eventual plateau.
- Link-heavy async: synchronous extraction/URL work dominates the event loop,
  corroborated by external stack sampling below. Increasing transfer concurrency
  alone provides little further throughput.
- Link-heavy threaded: useful scaling through eight workers, decline at sixteen;
  coordination lock wait is small. The precise cause of that decline remains open.
- Global discovery is inexpensive relative to extraction in these fixtures.
  Duplicate anchors still undergo URL resolution before per-page deduplication.

## 8. Hypotheses and next controlled experiments

1. Isolate duplicate-reference resolution cost inside the measured extractor.
   Compare anchor density and duplicate ratio at fixed body size and topology;
   then evaluate preserving semantics while avoiding repeated resolution within
   one document. Canonicalization, fragments, relative bases and order must remain
   covered by existing tests. No such optimization is implemented here.
2. Use allocation tracing to attribute counts/bytes to URL parsing and DOM
   construction/destruction. The sample shows allocator activity but does not
   establish which reserve-capacity or allocator change would help.
3. Measure threaded worker CPU scheduling and server CPU separately around
   4/8/16 workers. Move the fixture to a dedicated host or calibrated server before
   claiming client CPU or server saturation.
4. Repeat longer and more interleaved trials under controlled power conditions
   to quantify instrumentation overhead and concurrency-one framework overhead.
   The rejected host-interrupted runs make this especially important.
5. After confirming extraction cost across more realistic duplicate ratios,
   consider a bounded processing-offload experiment for async. That would be a
   later architectural change, with ordering/memory tradeoffs, not this phase.

Response-body string growth, link vectors, global seen-node allocation and
`ActiveTransfer` allocation remain inspected candidates. Neither body-buffer
reallocation nor transfer allocation was established as a dominant cost.

## 9. External profiler and memory observations

`/usr/bin/sample` and `/usr/bin/time -l` were available and executed. `perf` was
not found; Linux commands are documented but were not run. Instruments was not
used and no profiler package was installed.

A separate Release async/16 link-heavy benchmark ran 50 repetitions without
internal profiling. `/usr/bin/sample <pid> 5 1 -mayDie -file ...` captured five
seconds at a 1 ms interval. Of 4,304 main-thread stack observations, 4,138 were
in `process_response`/extraction work. Large sibling subtrees included the URL
resolution path (1,840 extractor-callsite observations), libxml document parsing
(1,562), and document cleanup (437). These are sampled stack observations, not
exact time percentages or independent allocation counts. The all-thread leaf
list also contains blocked system workqueue threads; their `__workq_kernreturn`
counts must not be mistaken for the crawler's CPU hotspot.

Active leaves include small-allocation/free routines, `htmlCurrentChar`,
`htmlParseCharData`, libcurl URL formatting/parsing, and string copies. This
corroborates the extraction category and motivates allocation attribution;
it does not justify blindly adding `reserve()` to response buffers or containers.

A separate ten-repetition `/usr/bin/time -l` run measured **2.25 s real,
2.12 s user, 0.06 s system**, maximum RSS **19,611,648 bytes (18.70 MiB)**,
and peak memory footprint 10,192,512 bytes. These are whole-client-process
metrics, including validation/control/output, excluding the separate server.
They support substantial CPU work in this workload, not per-worker utilization.
RSS is a high-water measure, not a leak test or allocation breakdown.
Raw outputs: `results/step9-async-sample.txt`, `step9-sampled-benchmark.txt`,
`step9-time.txt`, and `step9-time-benchmark.txt`.

## 10. Verification

The full standard suite passes 46/46. ASan/UBSan passes 46/46; the final
benchmark diagnostic/server CLI changes are additionally checked by the three
benchmark tests. ThreadSanitizer passes the selected threaded, profiling and
benchmark integration tests; final profiling/benchmark checks are repeated after
the diagnostic change. No sanitizer findings were reported. The initial new
profiling test used a production connection timeout against the deliberately
unreachable fixture and exceeded its harness timeout; setting the same 100 ms
fixture timeout used by existing tests resolved that test issue.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-tsan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan \
  -R 'core.profiling|core.thread_pool_crawler|benchmark.integration' --output-on-failure
```

Tests cover absent disabled summaries, enabled zero-work and depth boundaries,
URL/depth/status/failure equivalence, worker-local merges, logical counter
invariants, active caps, async completions, profile CSV blanks and fixture CLI
validation. There are no timing-based performance pass/fail thresholds.
