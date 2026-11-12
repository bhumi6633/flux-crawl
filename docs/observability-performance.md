# Observability overhead experiment

Measured on November 11, 2026 using the Release build on macOS 15.0.1 arm64,
Apple Clang 16.0.0 and Boost 1.90.0. These are actual local measurements,
not acceptance thresholds or a universal performance ranking.

## Method

Ran `python3 tools/observability_benchmark.py --repeats 3` after builds, sanitizer
tests and the demo finished. Earlier exploratory runs during compilation were
discarded. Each case creates the same three-origin deterministic fanout fixture:
12 branches × 120 leaves, 15 ms per-response delay, repeated discoveries, one
retry for every 19th leaf, 12 async slots, four per host, and 100/25 queue
watermarks. All 15 runs produced **1529 resource attempts**, with the same
1453 logical resources. No robots, topology collection or persistence was
enabled in this experiment. Case order rotates between repeats.

The three baseline cases are disabled, enabled with zero clients, and enabled
with one consuming WebSocket client at 500 ms. Additional one-client cases
compare 100 and 1000 ms. Final-state linger is zero in measured runs, so process
grace time does not inflate overhead. Final-snapshot delivery is tested separately.

Elapsed is process-start-to-exit wall time including initialization and teardown;
throughput = resource attempts / elapsed. POSIX `wait4` measures crawler child
user+system CPU seconds and peak RSS (converted to MiB below). Python fixture and
client CPU/RSS are excluded, as is React/browser rendering. The host was free of
other task builds/tests; unrelated machine activity is uncontrolled.

## Medians of three runs

| Mode | Interval | Elapsed s | Attempts/s | Child CPU s | Peak RSS MiB | Received snapshots |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| disabled | — | 2.68429 | 569.61 | 0.25114 | 10.672 | 0 |
| no-clients | 500 | 2.64222 | 578.68 | 0.28823 | 11.609 | 0 |
| client | 500 | 2.67519 | 571.55 | 0.26548 | 11.578 | 6 |
| client | 100 | 2.63869 | 579.45 | 0.29026 | 12.328 | 26 |
| client | 1000 | 2.68119 | 570.27 | 0.29221 | 11.125 | 3 |

## Interpretation and default

The observed elapsed-time ranges overlap; enabled runs were sometimes faster
than disabled runs. This is run-to-run variation, not evidence that telemetry
accelerates crawling. Three short, delay-dominated runs cannot establish a
precise overhead percentage, and the CPU/RSS variation should not be hidden.

Keep **500 ms** as the default: the one-client experiment delivered six
snapshots versus 26 at 100 ms and three at 1000 ms, without an observed
elapsed-time regression relative to disabled mode on this workload. It provides
a useful two-updates-per-second console without the message volume of 100 ms.
The measurements do not prove 500 ms is universally optimal or that overhead is
zero. CPU-heavy extraction, very large host populations, and browser graph
rendering warrant separate measurements. Topology is already opt-in and capped.

## Individual measurements

| Repeat | Mode | Interval ms | Elapsed s | Attempts/s | CPU s | Peak RSS bytes | Snapshots |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | disabled | — | 2.82086 | 542.03 | 0.29001 | 12222464 | 0 |
| 1 | no-clients | 500 | 2.68482 | 569.50 | 0.24433 | 10813440 | 0 |
| 1 | client | 500 | 2.63473 | 580.32 | 0.26548 | 13090816 | 6 |
| 1 | client | 100 | 2.61423 | 584.87 | 0.31478 | 12926976 | 26 |
| 1 | client | 1000 | 2.62575 | 582.31 | 0.29221 | 13205504 | 3 |
| 2 | no-clients | 500 | 2.64222 | 578.68 | 0.30171 | 12173312 | 0 |
| 2 | client | 500 | 2.67519 | 571.55 | 0.28154 | 12140544 | 6 |
| 2 | client | 100 | 2.64663 | 577.72 | 0.29026 | 13041664 | 26 |
| 2 | client | 1000 | 2.68119 | 570.27 | 0.30111 | 10960896 | 3 |
| 2 | disabled | — | 2.68429 | 569.61 | 0.20435 | 11190272 | 0 |
| 3 | client | 500 | 2.68838 | 568.74 | 0.24207 | 11878400 | 6 |
| 3 | client | 100 | 2.63869 | 579.45 | 0.25934 | 11763712 | 26 |
| 3 | client | 1000 | 2.68724 | 568.99 | 0.23455 | 11665408 | 3 |
| 3 | disabled | — | 2.67036 | 572.58 | 0.25114 | 11059200 | 0 |
| 3 | no-clients | 500 | 2.62119 | 583.32 | 0.28823 | 12550144 | 0 |

Raw transient JSONL captures remain outside Git; the complete measured values
are recorded above and the reproduction tool is tracked. No machine-specific
paths or runtime databases are committed.
