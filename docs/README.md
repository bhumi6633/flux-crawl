# FluxCrawl documentation

Choose a reading path rather than reading every historical report in order.
The root [README](../README.md) is the project entrance; these documents explain
or substantiate its claims.

## Understand and present the project

| Document | Purpose |
| --- | --- |
| [Technical one-pager](project-one-pager.md) | Architecture, hardest problems, findings and scope before an interview |
| [URL design walkthrough](design-walkthrough.md) | Component ownership from discovery to durable completion |
| [Design notes](design-notes.md) | Invariants, durability and telemetry boundaries |
| [Interview guide](interview-guide.md) | Short explanations, 30 questions, real bugs and demo talking points |
| [Resume and portfolio material](resume-bullets.md) | Defensible bullets, evidence map, GitHub metadata and two descriptions |
| [Local demo and resume commands](demo.md) | Existing deterministic systems demo and reproducible recovery examples |

## Inspect the evidence

| Document | Purpose |
| --- | --- |
| [Final benchmark report](benchmark-report.md) | Release measurements, environment, variance, figures and selected raw data |
| [Certification](certification.md) | Fuzz/sanitizer/chaos/stress/recovery evidence and remaining tag gates |
| [Packaging verification](packaging-verification.md) | Step 20 command, build, frontend and link checks |
| [0.1.0 candidate notes](release-notes-0.1.0.md) | Concise release scope and validation limits |
| [Profiling guide](profiling.md) / [historical report](../profiling-report.md) | Instrumentation and measured bottleneck attribution |
| [Optimization experiment](optimization-report.md) | Matched hypothesis, control workloads, rejected candidates and tradeoffs |

## Follow a subsystem

* [Host scheduling](host-scheduling.md), [retries](retries.md), [robots policy](robots.md)
* [Backpressure](backpressure.md), [exact/Bloom deduplication](deduplication.md)
* [Checkpointing](checkpointing.md), [SQLite persistence](persistence.md)
* [Observability design](observability.md), [validation](observability-validation.md),
  [historical overhead measurements](observability-performance.md)

Subsystem reports preserve phase-specific measurements and baseline test counts;
they do not supersede final certification. In particular, the historical vector
extractor optimization and the current bounded streaming path are distinct.
The current [benchmark report](benchmark-report.md) is the selected performance
baseline; no unsupported historical speedup is inferred from unmatched runs.

## License and provenance

[MIT license](../LICENSE), [third-party notices](../THIRD_PARTY_NOTICES.md),
[changelog](../CHANGELOG.md). Generated build output, databases and scratch logs
are ignored. Curated CSV/JSON data and figures under this directory are intentional
evidence. No verified dashboard screenshot or remotely executed green CI status
is included.
