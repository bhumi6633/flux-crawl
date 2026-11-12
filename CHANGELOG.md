# Changelog

## 0.1.0 — prepared release candidate

A C++20 concurrency and performance laboratory with sequential blocking,
fixed-worker blocking, and single-event-loop libcurl multi execution.

* Shared canonical URL identity, HTML discovery, host scheduling, retry/backoff,
  literal-prefix robots policy, bounded admission, and exact deduplication.
* Snapshot and SQLite recovery, including cross-engine at-least-once recovery;
  optional Bloom prefilter with exact SQLite verification.
* Deterministic benchmark/profiling tools and a local, bounded React observatory.
* Parser fuzz targets, deterministic HTTP/write/transaction faults, lifecycle and
  recovery certification, compiler/sanitizer CI, and source release tooling.

This is an experimental systems project, not an internet-facing service. Tagging
is subject to the gates and actual results in `docs/certification.md`.
