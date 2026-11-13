# FluxCrawl 0.1.0 — prepared candidate notes

FluxCrawl implements one crawl workload using three C++20 execution models:
sequential blocking I/O, a fixed blocking thread pool and a single crawl-thread
libcurl multi loop. It is a network-concurrency/performance experiment with
controlled semantics and measurements.

## Included

* Shared canonical URL identity and libxml2 discovery; origin-aware scheduling,
  concurrency/rate limits, delayed retries and opt-in literal-prefix robots policy.
* High/low admission bounds with disk-backed deferred discovery; exact memory or
  SQLite membership and an optional Bloom negative prefilter.
* Snapshot and incremental SQLite state, cross-engine resume, committed-prefix
  recovery and at-least-once active-attempt reissue.
* Deterministic benchmark/profiling tools and selected Release data/figures;
  a bounded loopback REST/WebSocket server and React systems console.
* Parser fuzzing, HTTP/storage fault injection, stress/recovery/lifecycle checks,
  compiler/sanitizer CI definitions and deterministic source packaging.
* Project overview, URL lifecycle walkthrough, local demo, interview guide and
  evidence-linked resume/portfolio material.

## Validation and release decision

Recorded local certification passed 76 normal tests, 78 Release/heavy tests,
ASan/UBSan, TSan checks, over six million final fuzz inputs and a 600-second
lifecycle soak. The [certification table](certification.md#final-execution-results)
provides exact suite counts and limitations. The [packaging record](packaging-verification.md)
adds final documentation command/build checks. Performance claims remain scoped
to [the measured environments and fixtures](benchmark-report.md).

**Hold the release tag:** remote compiler/sanitizer matrix execution, Linux leak
detection and actual browser visual QA remain unexecuted gates. These notes do
not announce a published release. No push, tag or repository-settings mutation is
performed by this packaging phase.

## Scope

Single-process HTTP(S), no JavaScript rendering or content cache. Origin policy
is not DNS/IP politeness; redirect hops retain the original scheduler claim.
Robots is a documented subset. Pending reservations and telemetry are bounded;
retained records, hosts and disk state can grow. SQLite has one logical owner,
and recovery can repeat HTTP requests. See [limitations](../README.md#limitations)
and [dependency notices](../THIRD_PARTY_NOTICES.md).
