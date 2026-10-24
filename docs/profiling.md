# Profiling FluxCrawl

Benchmarking measures throughput and latency; profiling helps explain the result.
Use a Release build for experiments. Internal instrumentation is portable C++20,
uses `steady_clock`, and requires no operating-system profiler API.

## Run a controlled experiment

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
mkdir -p results
python3 tools/benchmark_server.py --port 8080 --scenario latency-bound \
  --pages 100 --fanout 99 --body-bytes 4096
```

On a laptop, keep the system awake during measurement. On macOS you can prefix
each benchmark command with `/usr/bin/caffeinate -i`; the assertion ends when
that command exits. Do not accept a run interrupted by sleep as a throughput
sample. Validation rejects failed requests but cannot detect every host pause.

In another terminal, run both modes, in this order:

```sh
build-release/fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario latency-bound --engine all --workers 1,2,4,8,16,32 \
  --concurrency 1,2,4,8,16,32,64 --runs 3 --warmups 1 \
  --output results/network-normal.csv
build-release/fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario latency-bound --engine all --workers 1,2,4,8,16,32 \
  --concurrency 1,2,4,8,16,32,64 --runs 3 --warmups 1 --profile \
  --output results/network-profile.csv
```

Stop that server before starting a different scenario. For processing experiments,
start `--scenario link-heavy` (default 100 pages, fanout 99, delay 0 ms,
64 KiB bodies, 1,000 additional shared anchors per page). Use the same client
commands with `--scenario link-heavy`, both sweeps `1,2,4,8,16`, and fresh output
filenames. `mixed-latency` uses page ID modulo four to select 5/20/50/100 ms;
its `delay_pattern` overrides `delay_ms`, including explicit delay overrides.
All seven presets support the existing page/fanout/body-size overrides.
Too-small bodies are rejected rather than truncating generated links.

The root exposes enough ready work for these sweeps. Extra shared links point to
the root and first-level pages, preserving the original tree's shortest depths.
The server uses concurrent request threads; sleeps release the Python GIL. Its
`server_peak_active` counts handlers doing delay/body work, ending before response
transmission. It is not a count of sockets or proof of unlimited server capacity.
Low-delay body generation, Python scheduling, connection setup and the shared
machine can limit throughput. Profile the server independently before attributing
a plateau to the client. Workload metadata v3 describes shared anchors and delay
pattern; the client still accepts v1 and v2 metadata.

## Architecture and definitions

Library callers opt in with `CrawlOptions::profile = true`; results have an
optional `ProfileSummary`. The CLI currently exposes this through **benchmark
mode**. `BenchmarkSample` carries the optional summary after full workload
validation. Ordinary crawl/benchmark calls produce no detailed profile.

| Field | Measurement boundary |
| --- | --- |
| Request latency (existing metric) | Blocking fetch or async attached-transfer lifetime until its completion is observed. Includes HTTP work; async observation can be delayed by other completion processing. Not pure network wait. |
| `extraction_ms` | Entire `HtmlLinkExtractor` call: DOM parsing, XPath, href extraction, URL resolution/normalization, per-page deduplication, vector construction, and extractor cleanup. Not libxml CPU time alone. |
| `discovery_ms` | Iteration of extracted unique links, same-host filter, global seen lookup/insertion and enqueue. Excludes resolution, which is inside extraction. |
| `frontier_ms` | Sequential/async scheduler acquisition attempts (including unsuccessful attempts). Threaded acquisition bookkeeping plus publication while holding the coordination lock, including discovery and result updates. Excludes initial state construction and final aggregation. Engine boundaries differ deliberately; do not compare this column as identical work. |
| `lock_wait_ms` | Sum of elapsed time immediately around explicit acquire/publish mutex lock calls, including uncontended lock cost. Excludes CV reacquisition and fatal exception cleanup (failed crawls return no profile). |
| `worker_idle_ms` | Sum of each `condition_variable::wait` or `wait_until` call through return with the mutex reacquired. Includes spurious wakeups and reacquisition, which cannot be separated portably. |
| `worker_activity_ms` | Sum of claimed-request intervals through publication: HTTP, extraction, publication lock wait, and discovery. Not CPU utilization. Acquire-side lock wait and time outside requests are excluded. |
| `multi_wait_ms` | Time in `curl_multi_poll`, including poll overhead and readiness/timer waits. Excludes no-active-transfer sleeps until host rate deadlines. Not all network waiting: transfers also progress during other loop work. |
| `completion_processing_ms` | Drain of `curl_multi_info_read`, detach, metadata conversion, extraction, discovery, record publication and completed-transfer destruction. Excludes `curl_multi_perform` and refill. |
| `refill_ms` | Every refill invocation, including frontier selection, transfer allocation/configuration and multi attachment. Setup-failure publication is included if it occurs. |

Counters include extraction calls, unique-per-document links considered globally,
accepted links (excluding seed), worker requests, explicit lock acquisitions,
CV waits/returns, loop iterations, multi waits, completions, successful transfer
attachments (`refills`, including initial fill), and maximum active work. Active
workers are claimed through publication; active transfers are attached until
completion removal. Neither maximum is average utilization. A zero-budget crawl
has no worker records, no transfer attachments and no event-loop iterations.

Each worker owns a preallocated profile slot and writes only that slot. Profiles
are merged after join; peak worker activity uses the existing coordination lock.
There are no profiler locks or shared atomic duration counters. Worker slots are
allocated only when profiling is enabled. Disabled timers check a null pointer
without reading the clock; disabled result optionals contain no live profile or
worker allocation. Optional storage has a fixed object-size cost. No timestamp
is taken per anchor. Instrumentation still perturbs enabled runs (clock calls,
branches, counter updates, possible adjacent-worker cache contention); treat
normal runs as headline throughput and profile runs as attribution evidence.

Durations **overlap**. Thread sums can exceed crawl wall time. Extraction and
discovery are nested inside completion processing; threaded discovery is nested
inside frontier work and activity. Do not add columns to reconstruct elapsed
time or subtract their sum to invent an “other” or “network” bucket. These are
elapsed operation durations, not CPU samples. No average utilization or process
CPU time is inferred from them.

## Output and CSV contract

The console prints each trial's common and applicable engine profile, plus
per-worker request/activity/idle rows. Existing median throughput and elapsed
summaries remain unchanged; request percentiles remain per trial.

The original 34 CSV columns retain their names/order. Append-only additions are:

```text
shared_anchors,delay_pattern,profile_enabled,
extraction_ms,discovery_ms,frontier_ms,extractions,links_considered,links_accepted,
lock_wait_ms,worker_idle_ms,worker_activity_ms,lock_acquisitions,cv_waits,
worker_wakeups,worker_requests,max_active_workers,
multi_wait_ms,completion_processing_ms,refill_ms,event_loop_iterations,
multi_waits,completions,refills,max_active_transfers,worker_profiles
```

There are 60 columns. Disabled profile durations/counters are blank; only
`profile_enabled=0` is populated. Engine-inapplicable fields are blank. Measured
zeroes are meaningful, e.g. enabled zero-budget crawls. Worker detail is available
in the library result and console; CSV contains merged totals and worker count.
Readers should address columns by name and tolerate appended fields. CSV output
is exclusively created and never overwrites existing files. All profile trials
retain URL/depth, body-size, dedup, budget, status and server-count validation.

For scaling, compare median attempts/s at each configuration against the same
engine at concurrency one. `rate(N)/rate(1)` is speedup; dividing by N gives
scaling efficiency relative to that configuration, not CPU efficiency. Compare
threaded and async at equal limits with the caveat that a worker and an attached
transfer represent different units of activity. Use repetitions consistently;
never select the fastest trial. A flat curve alone does not identify its cause.

## External tools

Run external profiling separately from the headline sweep. Keep raw files in
ignored `results/`. No external profiler is required by builds or CI.

On macOS, the system tool is `/usr/bin/sample` (another executable called
`sample` can precede it on PATH). With the link-heavy server running:

```sh
build-release/fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario link-heavy --engine async --concurrency 16 --runs 100 \
  > results/sample-benchmark.txt &
crawl_pid=$!
/usr/bin/sample "$crawl_pid" 5 1 -mayDie -file results/async-sample.txt
wait "$crawl_pid"
/usr/bin/time -l build-release/fluxcrawl benchmark \
  --seed http://127.0.0.1:8080/page/0 --scenario link-heavy \
  --engine async --concurrency 16 --runs 10 > results/time-benchmark.txt \
  2> results/time-macos.txt
```

`sample` reports stack observations, including blocked stacks, not exact CPU
percentages. Inspect the calling context of hot leaves and sampling duration.
`time -l` supplies process user/system CPU time and maximum resident set size
(bytes on macOS). Those cover the entire CLI process, including initialization,
control requests, validation and output; they are not per-crawl durations.
Instruments Time Profiler/Allocations can investigate allocation counts and
sizes later; no GUI automation or platform-specific production code is needed.

On Linux with perf and suitable permissions, against the same server:

```sh
perf stat -o results/perf-stat.txt -- build-release/fluxcrawl benchmark \
  --seed http://127.0.0.1:8080/page/0 --scenario link-heavy \
  --engine async --concurrency 16 --runs 20
perf record -g -o results/perf.data -- build-release/fluxcrawl benchmark \
  --seed http://127.0.0.1:8080/page/0 --scenario link-heavy \
  --engine async --concurrency 16 --runs 20
perf report -i results/perf.data
/usr/bin/time -v build-release/fluxcrawl benchmark \
  --seed http://127.0.0.1:8080/page/0 --scenario link-heavy \
  --engine async --concurrency 16 --runs 10
```

Unwinding quality depends on platform/toolchain. A separately labeled optimized
build with debug symbols/frame pointers can help; keep its results distinct.
Linux perf commands are guidance, not a claim they were executed on macOS.
Allocation candidates from code inspection are response-string growth, URL
string creation, parser vectors/sets, global seen nodes, and per-request
`ActiveTransfer`/curl objects. They remain hypotheses until sampled or counted;
this phase adds no reserve-capacity or allocator optimizations.

See [the initial measured report](../profiling-report.md) for this machine's
results and limitations.

## Host scheduling (Step 11)

The original benchmark runner rejects configured host caps/rates, so its existing
CSV always represents unlimited per-origin policy. `CrawlOptions::scheduling`
applies to library crawls and the regular crawl CLI. Existing profiling fields
remain available to library callers: threaded idle time includes rate-deadline
waits; async `multi_wait_ms` excludes rate-only sleeping with no active transfers.
No separate host-block/rate-wait counters are claimed. Host queue publication is
inside threaded frontier time and async completion time; sequential publication
is outside frontier time. See [host scheduling](host-scheduling.md) for the
separate multi-origin scenario and its explicitly recorded policy metadata.
