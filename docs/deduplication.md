# Scalable exact deduplication
## Pre-implementation MemoryExact baseline
Release, Darwin arm64, three fresh processes per point. Existing unordered_set<Url>,
canonical https://host.test/page/N URLs generated individually outside each timed
operation. RSS includes the process and a transient URL, not a prebuilt input vector.
100k lookups per sample, 50% hits. Per-operation steady-clock sampling adds overhead.
Reserve time is separate; growth rehashes exclude initial reserve.
| URLs | Reserve | Insert ms | Lookup 100k ms | RSS bytes | Buckets | Load | Growth rehashes |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10000 | 0 | 0.919 | 4.662 | 5095424 | 12853 | 0.778 | 13 |
| 10000 | 1 | 0.868 | 4.962 | 4964352 | 10007 | 0.999 | 0 |
| 100000 | 0 | 20.055 | 12.591 | 25673728 | 102877 | 0.972 | 16 |
| 100000 | 1 | 15.694 | 16.069 | 24625152 | 100003 | 1.000 | 0 |
| 500000 | 0 | 158.119 | 18.158 | 121618432 | 823117 | 0.607 | 19 |
| 500000 | 1 | 86.642 | 17.056 | 112279552 | 500009 | 1.000 | 0 |
| 1000000 | 0 | 370.023 | 22.813 | 240484352 | 1646237 | 0.607 | 20 |
| 1000000 | 1 | 216.198 | 26.024 | 222183424 | 1000003 | 1.000 | 0 |
