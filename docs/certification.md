# FluxCrawl 0.1.0 certification

This is an evidence log for a prepared **0.1.0 release candidate**, not a claim
that every environment or input has been proven safe. No release tag or remote
publication is created by this phase. Results below distinguish executed checks,
unsupported tooling and remaining gates.

## Repository audit

The baseline at `82ccc87` built successfully and passed 70/70 tests. The audit
covered all three engines, shared URL/HTML/robots/retry policy, scheduler, pressure,
seen backends, spool, snapshot/SQLite durability, telemetry server/frontend,
benchmarks, test organization, build dependencies, CI and licensing.

High-risk boundaries are untrusted parser input, owner-serialized scheduling,
active-lease cleanup during exceptions, delayed-only termination, uncommitted
exact membership, durable commit order and WebSocket teardown. Previously
under-tested areas were parser fuzzing, real storage write failures, repeated
same-process lifecycles and a long bounded-graph soak. Platform-sensitive code
includes POSIX file creation/fsync/rename/flock, loopback port resources, Apple
standard-library feature availability and sanitizer runtimes.

The audit resulted in these focused fixes:

* IPv6 origin telemetry no longer wraps an already bracketed host a second time.
* Sequential exception unwinding reacquires the telemetry mutex before releasing
  a scheduler lease, preventing unsynchronized sampler access to its gauges.
* Modern standard libraries use `atomic<shared_ptr>`; older Apple libraries keep
  the supported atomic free-function fallback behind a feature test.
* GCC warnings exposed an implicit Bloom arithmetic conversion, aggregate
  defaults and a shadowed restore variable. These were fixed explicitly.
  An initializer-list/optional-copy diagnostic in a test was avoided with explicit
  vector insertion; no broad warning suppression was added.
* Dashboard JSON is checked for required objects, finite/nonnegative numeric
  fields, nullable metrics, array caps and valid graph references before rendering.
* CLI help is grouped and exit codes distinguish configuration, runtime and
  persistence/resume errors. Version 0.1.0 has one runtime source of truth.
* Unbuilt, attributed coursework/examples under lab4/lab5 were removed after
  verifying no FluxCrawl build/test depended on them. MIT LICENSE is retained;
  direct/transitive dependency provenance is summarized in THIRD_PARTY_NOTICES.md.

TODO/FIXME/HACK/XXX and tracked path/secret audits found no unresolved release
TODOs or credentials. The discovery spool's `XXXXXX` is an intentional mkdtemp
pattern. Core hot-path output is limited to intentional failure diagnostics;
CLI/benchmark tables are intentional user output.

## Invariants and test map

| Invariant | Evidence |
| --- | --- |
| A canonical URL is accepted globally at most once | URL, state, dedup and engine-equivalence suites; adversarial anchors; lifecycle set comparisons |
| A retry does not create another logical URL | Retry integration, HTTP chaos, 20 delayed-only cycles, exact backend parity |
| Resource attempts never exceed max_pages | Reservation/page-budget tests across all engines; crash/resume budgets; lifecycle checks |
| Global/per-origin active claims remain within configured limits | Host/crawler tests, concurrency stress, telemetry assertions; robots/retries included |
| Robots-blocked resources are not fetched | Robots integration and manual soak fixture request-set check |
| Pressure does not silently drop accepted/deferred work | Bounded/unbounded URL-set parity, high-fanout tests, durable spool recovery |
| Delayed-only work prevents premature termination | Retry integration; 20 repeated delayed-only waits with elapsed/CPU checks |
| No normal termination while work remains | All full-completion equivalence tests; exceptions are budget/stop/fatal persistence outcomes |
| Checkpoint/resume retains unfinished logical work | Cross-engine graceful and active-snapshot recovery, corruption corpus, failed write/replace tests |
| SQLite restores committed state and does not resurrect terminal rows | Transaction/corruption/crash-prefix tests; FULL/I/O injection; completed resume |
| Bloom false positives cannot lose URLs | Exact backend parity, intentionally tiny/saturated Bloom tests and 100k persistent restart |
| Telemetry does not affect crawl results | Snapshot parity, live engine counts, disconnected/slow clients, same-process lifecycle soak |
| Destruction releases workers/samplers/server/transport state | Explicit joins/RAII, TSan, repeated lifecycle checks, descriptor/RSS sampling |

Acceptance is discovery-time identity, not a successful HTTP response. Retry
attempts count toward max_pages; infrastructure robots attempts are separate.
A durable active attempt may be fetched again after a crash. These qualifiers
are essential; stronger exactly-once/network claims would be false.

## Test taxonomy and commands

The existing vertical-slice layout is retained. CTest labels cover unit,
integration, concurrency, recovery, persistence, stress and benchmark-smoke.
Optional Clang fuzz builds add fuzz/fuzz-smoke. Frontend tests use Node's test
runner and strict TypeScript, separately from CMake.

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -L concurrency --output-on-failure
ctest --test-dir build --repeat until-fail:10 \
  -R 'certification.lifecycle|retry.stress|robots.stress|core.observability|checkpoint.integration' \
  --output-on-failure -j 2
python3 tools/repeat_certification.py --rounds 3
```

Heavy 10k/100k tests require `FLUXCRAWL_SLOW_TESTS=ON`. The ten-minute soak and
benchmarks are manual. CI has a dispatch-only heavy job; ordinary commits do not
run a ten-minute crawl. Run large network suites one at a time on an awake local host. CTest network
fixtures now share a resource lock, so `ctest -j` can parallelize independent
unit work without overlapping unrelated timing-sensitive network fixtures.
Actual concurrency within each fixture remains unchanged.

## Parser fuzzing

Four optional Clang libFuzzer targets instrument project code with ASan/UBSan:

| Target | Exercised boundary / property |
| --- | --- |
| url | Arbitrary bytes, canonical reparse/idempotence, equality/hash, stable fragment removal |
| resolve | Valid/arbitrary bases and arbitrary relative references, successful-result URL invariants |
| robots | Untrusted bounded body, grouping/token selection/prefix matching, deterministic decisions |
| checkpoint | Full deserialization, malformed-version/count/state rejection and deterministic accepted-state round trip |

The checked-in seed corpus is small, readable and includes normalized/default
ports, IPv6, fragments, relative/query references, robots allow/deny groups and
valid/corrupt checkpoints. Checkpoint seeds include truncated, wrong-version,
invalid-URL, missing-field, duplicate-pending, impossible-retry and absurd-count
cases. A normal unit test rejects all seven corrupt cases before restoration.
Trusted backend JSON serialization is not pointlessly fuzzed; the external
frontend JSON boundary has malformed-message tests.

```sh
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
  -DFLUXCRAWL_ENABLE_FUZZING=ON -DFLUXCRAWL_OBSERVABILITY=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz -j 4
python3 tools/fuzz_smoke.py --seconds 15
ctest --test-dir build-fuzz -L fuzz-smoke --output-on-failure
```

Fuzzing is not mandatory for GCC or ordinary builds. Corpora/artifacts/logs are
copied into ignored build directories. Smoke runs have time/input/RSS limits;
longer campaigns are manual. The first URL smoke exceeded the 512 MiB RSS limit
with a 235 MiB ASan quarantine and about 24 MiB live allocations, mostly fuzzer
runtime storage. The emitted empty input was not a parser-specific reproducer.
Setting the harness quarantine to 32 MiB kept the same 512 MiB RSS cap and passed
subsequent runs; project input assertions and sanitizer detection stay enabled.

Initial successful 15-second campaigns executed 3,332,689 URL, 1,635,700 resolve,
544,009 robots and 401,085 checkpoint inputs, with zero crash artifacts. Final
campaign counts after all changes are recorded in the results table below.

## Faults, chaos and recovery

FaultPlan is a test-only fail-after-N hook; no production randomness or general
injection framework was introduced. Checkpoint writes reuse the existing
replacement hook and a narrowly scoped pre-write hook. Both failures preserve the
previous readable primary and clean owned temp files. SQLite uses a scoped wrapper
of the platform VFS, returning real SQLITE_FULL and SQLITE_IOERR_WRITE errors
while delegating normal I/O. Reopening sees only the committed prefix, then a
successful reapplication proves recovery. End-to-end cases additionally fail the
first runtime commit for each engine using both FULL and write-I/O errors, with
Bloom+SQLite and high=5/low=2. They assert visible persistence errors, bounded
further claims, an error telemetry state, an intact sequence-zero prefix, and
successful Async resume to all 16 logical resources without resurrected terminal
work. The test VFS identifies the small metadata write by its sequence text,
rather than assuming a platform-specific initialization write count. Existing mutation hooks also exercise
rollback between seen/work updates and before final commit.

The HTTP fixture deterministically closes before headers, truncates a declared
body, delays one response beyond its timeout, emits 429/503 then recovers,
exhausts 500 retries, follows a bounded redirect chain, loops redirects, omits
Content-Type, exceeds the body limit and serves malformed HTML. All three engines
continue sibling work. Oversized bodies and redirect loops release active slots
and do not receive inappropriate retries. A separate adversarial test exercises
10,000 repeated/fragment anchors, entities, empty href, a 20k href and IPv6 origins.

Existing snapshot and SQLite suites certify Sequential→Async,
Threaded→Async and Async→Threaded recovery, active reissues, clean stops,
SIGKILL windows, delayed retry restoration, budget preservation, completed rows,
logical corruption and single-owner exclusion. At-least-once active reissues are
expected; they do not create duplicate logical seen entries. Engine/backend set
comparisons cover chains/depth barriers, fanout, duplicate links, multi-origin
and redirected discovery. Bloom saturation degrades lookup efficiency, not exactness.

## Bounded-graph soak and idle behavior

```sh
python3 tools/certification_soak.py --seconds 600
```

One child process repeatedly creates, runs and destroys sequential, 16-worker
threaded and 32-slot async crawlers, plus bounded telemetry and HTTP server
lifecycles. The fixed graph has three origins, robots-blocked leaves, duplicate
links, initial transient failures, pressure high=5/low=2, per-origin cap=1 and
periodic rate limiting. Each session compares its resource set and final metrics
against the reference. Samples record elapsed time, sessions, current RSS,
descriptors, fixed logical set size, attempts and throughput. Session starts are
paced to at most five/second; this tests retained state, not port exhaustion.

The completed run lasted at least 600 seconds: **2,938 sessions, 58,763 resource
attempts**, 67,577 fixture requests including robots. Final RSS was **8,896,512
bytes**, and descriptors remained **3** after teardown. The fixture's per-origin
peak was [1,1,1]. No terminal failures or descriptor growth occurred. The fixed
fetched set remained 20 resources (plus eight robots-blocked logical resources).
RSS varies with allocator/OS behavior; this is stability evidence, not proof of
zero possible leaks. A thread-count time series was not collected; explicit joins,
RAII completion, repeated destruction and TSan cover thread lifecycle.

An earlier unpaced soak incorrectly equated robots fetch attempts with logical
bootstraps; that assertion was corrected to robots_origins, with retries counted
separately. Additional unpaced/overlapping runs encountered transport failures.
The host has 16,384 ephemeral ports (49152–65535) and 15 s TCP MSL; simultaneous
connection churn can exhaust shared loopback resources. The suites correctly
failed their logical assertions; those runs are not counted as PASS. The final
paced soak and isolated stress runs retain the original correctness checks.
Later full-suite attempts also overlapped repeated macOS maintenance Sleep /
DarkWake events confirmed in the power log. They produced timeout/connection
failures and a missed polling-based crash window; affected fixtures passed on
immediate awake rerun. These interrupted runs are not counted as clean passes.
Final local certification uses process-scoped idle/system sleep assertions on AC, without changing
persistent power configuration. This is a test-host precondition, not a claim of
continuous network progress while the OS suspends execution.

A separate wide-concurrency run (`python3 tools/certification_stress.py`) passed
four cases: threaded/async, two repetitions each, 32 slots, per-origin cap 16,
100 requests/s/origin with burst 16, 200 ms response delay and pressure 128/32.
Each completed exactly 2,021 logical URLs in 2,139 attempts, recovering all 118
transient errors. Observed global concurrency reached 32 in every case;
per-origin peaks remained at most 15 and peak admitted unfinished work was 128.
An exploratory 20 ms burst profile immediately following a heavy suite failed
with transport errors and budget exhaustion. That is a failed experiment, not a
certified profile. The controlled profile makes shared socket demand explicit;
it does not establish unlimited connection-churn tolerance.

A delayed-only async test performs 20 timer wakeups on one logical resource and
checks full completion, elapsed delay and process CPU below a broad busy-spin
threshold. It does not rely on a high-resolution throughput target.

## Final execution results

Final results and exact run counts are filled from the completed local logs.
A configured CI job is not reported as a remotely executed PASS.

| Check | Actual result |
| --- | --- |
| Baseline | PASS — 70/70 |
| Final normal suite | PASS — final 76/76 with awake host / fixture isolation, 104.25 s; earlier three complete 75-test repetitions (225 executions) also passed |
| Critical repetition | PASS — five ownership/recovery tests × ten repetitions (50 executions) |
| Full Release including opt-in heavy suites | PASS — final 78/78, 108.75 s; awake host, fixture resource locks and no competing suites |
| Apple Clang 16 project warnings | PASS — warning-as-error build |
| GCC 14 core/CLI (server disabled) | PASS — warning-as-error build, 73/73 tests plus final store/engine write-fault cases 2/2; native Boost dylib uses a different C++ ABI, so the local GCC server build was not attempted |
| ASan + UBSan | PASS — full 75/75, 136.55 s, plus the final store/engine write-fault cases 2/2; no project findings |
| TSan | PASS — full 13/13 concurrency-labeled tests, 169.75 s, plus final store/engine write-fault cases 2/2; no race reports |
| LeakSanitizer | NOT CERTIFIED locally — Apple runtime unsupported; alternate LLVM empty-program probe reports startup allocations; Linux CI check configured, not executed here |
| URL fuzz, 15 s budget | PASS — 3,819,339 executions, zero crash artifacts |
| Resolution fuzz, 15 s budget | PASS — 1,492,787 executions, zero crash artifacts |
| Robots fuzz, 15 s budget | PASS — 390,524 executions, zero crash artifacts |
| Checkpoint fuzz, 15 s budget | PASS — 395,404 executions, zero crash artifacts |
| Checkpoint corruption corpus | PASS — seven malformed states rejected, valid seed round trips |
| Real SQLite FULL/write failures | PASS — store rollback plus six end-to-end engine/error combinations; committed prefix restored and Async replay completed |
| Checkpoint write/replacement failure | PASS — old primary readable, owned temp cleaned |
| Controlled 32-concurrency stress | PASS — four cases, exact 2,021-URL sets, 118 recovered retries each, observed global peak 32, pending ≤128 |
| Threaded / async 10k combined stress | PASS — 10,021 resources, retries/robots/SQLite, pending ≤100 |
| Bloom+SQLite high-fanout restart | PASS — 100,101 URLs, exact fixture counts, pending ≤1000, cache ≤4096 |
| Cross-engine / snapshot / SQLite recovery | PASS — normal and sanitizer integration suites; durable-prefix and active reissue semantics asserted |
| Delayed-only idle CPU | PASS — 20 wakeups; TSan sample 0.650 s wall / 0.032 s CPU, same logical resource |
| Bounded-graph soak | PASS — ≥600 s, 2938 sessions, stable three descriptors, final RSS 8,896,512 bytes |
| Dashboard disconnect / slow clients | PASS — repeated abrupt disconnects, bounded in-flight write, stale drops and stalled-client disconnects |
| Frontend clean install/tests/typecheck/build | PASS — fresh clone, four parser tests, strict TypeScript and production Vite build |
| Live demo / actual frontend data | PASS — 71 snapshots, completed 733-seen/673-completed/60-blocked crawl, 39 recovered retries; actual final JSON accepted by frontend validator |
| Browser visual QA | NOT RUN — browser runtime reported no connected browser |
| Fresh clone | PASS — no local generated files, Release build and 75/75 tests; separate npm ci/test/typecheck/build passed |
| Official architecture benchmarks | PASS — 84 measured samples + excluded warmups, all logical validation passed |
| Separate feature/restore/telemetry measurements | PASS — actual selected raw data and generated figures in benchmark report |
| Source archive | Prepared from clean committed files; external SHA-256 accompanies archive, reproducibility checked locally |
| Remote CI execution | NOT RUN — matrix configured; do not treat configuration as execution |
| Coverage | NOT RUN — optional, no coverage percentage claimed |

Selected measured data are in [benchmarks/0.1.0](benchmarks/0.1.0/README.md),
with plots, environment, exact profiles, timing bases and variance in the
[benchmark report](benchmark-report.md). The [changed file list](certification-files.txt)
records this phase without copying generated scratch artifacts.


## CI and remaining release gates

The configured matrix covers Ubuntu GCC Release, Ubuntu Clang Debug and macOS
AppleClang Release, with project warnings as errors. Separate Linux Clang jobs
run ASan/UBSan with leak detection, TSan ownership/concurrency tests, bounded
libFuzzer smoke, and a Node 22 frontend install/test/typecheck/build. Heavy stress
is manual-dispatch only. No project race suppressions were added.

The GitHub-hosted jobs have **not been executed in this local session**. The
local browser runtime returned an empty browser list: **visual browser QA is
NOT RUN**. Built assets, actual RFC6455 transport, live final-state telemetry and
frontend schema acceptance are verified, but these do not establish visual layout.
LeakSanitizer is unsupported by the installed Apple sanitizer runtime; an alternate
LLVM empty-program probe itself reported startup allocations, so no local clean
LSan claim is made. Linux CI's explicit leak check remains a release gate.

**Tag decision:** prepare 0.1.0 and a clean source artifact, but hold the tag until
the remote compiler/sanitizer matrix (including Linux leak detection) and an actual
browser pass are green. Remaining limitations are in README and design-notes.md;
none is hidden behind a fabricated certification result. Next work is those
validation gates, not another major feature.
