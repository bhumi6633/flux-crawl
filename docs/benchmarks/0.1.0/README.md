# Selected certification data

These are intentional measured release-candidate inputs for the report/figures,
not scratch exports. All HTTP workloads are deterministic loopback fixtures.

* `samples.csv`: 84 architecture samples; four profiles, three engines, limits
  1/4/16, three measured runs and one excluded warmup. Sequential runs only at 1.
* `environment.json`: native dependency/compiler/host metadata and measured source
  revision; later commits add tests/reports/tooling, not different engine code.
* `backpressure.csv`: three runs per configured queue threshold on 373 URLs.
* `persistence.csv`: three runs per SQLite batch threshold on 121 resources.
* `dedup.csv`: 100k exact identities, three independent runs per backend/capacity,
  each with three 100k-query absent/present mixes. Peak RSS/CPU cover the whole
  native process, not an isolated container or the Python caller.
* `snapshot-resume.csv`, `sqlite-resume.csv`: synthetic 1k/10k state, 100 pending,
  three runs each. SQLite rows here restore MemoryExact; scalable exact/Bloom
  startup is separately measured in `dedup.csv`.
* `observability.jsonl`: 15 actual Release process measurements; three repeats of
  disabled, 500 ms/no-client, and one-client 100/500/1000 ms cases.
* `fuzz-smoke.jsonl`: final 15-second-per-target results, no artifact paths/data.
* `wide-stress.jsonl`: four controlled 32-concurrency threaded/async runs, each
  with 2,021 logical URLs and 118 recovered retries; exact fixture sets checked.
* `soak.tsv`: current RSS/descriptors after teardown on a fixed graph during the
  completed ten-minute same-process lifecycle soak.

Advanced timing includes process initialization/teardown; the architecture CSV
uses `CrawlResult.elapsed`. Different experiment families are not interchangeable.
Full transient logs, databases, generated build corpora and exploratory failed
runs stay outside Git; their findings/limitations remain documented.
