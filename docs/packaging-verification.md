# Step 20 packaging verification

This phase changes documentation only. No engine, CLI, dependency version,
license, workflow or dashboard implementation was changed. The runtime and
frontend remain version **0.1.0**, prepared candidate. GitHub description/topics
are supplied as text for manual settings; no remote publication or tag is made.

## Source and evidence audit

Reviewed the root/source/test/tool/frontend layout, current README, subsystem
design notes, final benchmark and certification reports, CLI crawl/benchmark
help, version header, dependency notices, CI workflow and the latest 150 history
entries. History retains the foundation→engines→measurement→scheduling→recovery→
storage→observability→hardening progression; it was not rewritten. Existing
assets are measured plots, not dashboard screenshots.

The README now leads with the experiment, diagram, engine comparison and scoped
results. Terminology consistently distinguishes deferred candidates, accepted
identities, pending work, active attempts and terminal resources. Source checks
confirmed these important boundaries:

* Threaded producers publish deferred work to disk and return to consumption;
  they do not wait for admission capacity.
* Admission applies exact identity before robots scheduling; deferred candidates
  remain unseen. Retries retain logical identity and consume resource budget.
* Robots bootstrap failures deny the origin after applicable retry handling.
  Policy is a documented subset; redirect-hop politeness is not separately enforced.
* Bloom maybe hits require exact checks; SQLite remains authoritative.
* Durable logical state permits cross-engine resume and repeated active HTTP
  attempts. Telemetry is immutable sampled visibility, not a journal.
* Historical vector-extractor optimization results are distinct from the current
  default bounded streaming extraction path.

Exaggerated-performance terminology was searched across README/design material;
no positive unsupported production, distributed, lock-free or zero-copy claim is
made. Resume bullets include a source/test/tradeoff evidence map. Portfolio and
application descriptions contain 114 and 58 words respectively.

## Executed command checks

| Command / workflow | Actual result |
| --- | --- |
| `cmake -S . -B build` | PASS |
| `cmake --build build` | PASS |
| `ctest --test-dir build --output-on-failure` | PASS — 76/76, 109.83 seconds |
| Release configure / build from README | PASS |
| `npm ci` | PASS — clean locked install |
| `npm run test` | PASS — four tests |
| `npm run typecheck` | PASS |
| `npm run build` | PASS — production Vite assets |
| Version / crawl / benchmark help | Inspected; runtime version 0.1.0 and documented flags match |
| Quick-start local crawl | PASS — 100 discovered / 100 resource attempts |
| Sequential snapshot stop → async resume | PASS — graceful stop, cumulative 100 attempts / completed graph |
| Threaded Bloom+SQLite stop → async resume | PASS — graceful stop, cumulative 100 attempts / completed graph |
| README Release benchmark | PASS — 21 measured CSV rows, three engines, three runs plus excluded warmups |
| Existing full dashboard demo | PASS — 733 seen, 673 completed, 60 blocked, 712 attempts, 39 recovered retries; no fetch/persistence errors |
| Local Markdown/image paths and heading fragments | PASS — 151 targets across 27 documents |
| `git diff --check` | PASS |

The initial restricted CTest attempt could not bind loopback sockets
(`PermissionError: Operation not permitted`). That is not counted as a test pass;
the complete rerun with loopback access produced the 76/76 result above. A
process-scoped sleep assertion kept the local macOS test host awake, without
changing persistent power settings. Network fixtures ran separately from the
documentation examples. The example benchmark verifies command usability and
logical checks; it does not replace the selected Step 19 performance dataset.

The demo reached peak admitted unfinished=60, with 14 pressure activations and
121 SQLite transactions. These sampled/operational totals can vary across runs;
logical fixture counts are the stronger validation. Its deliberately small
configured Bloom design capacity (300 vs 733 identities) was exceeded while
exact membership remained correct. No screenshot or browser layout was certified.

The exact README `latency-bound` fixture on port 8081 remained running for the
crawl, both recovery examples and the benchmark, then was stopped. New mktemp
state directories avoided overwriting prior state. CSV output exclusively creates
a new file. Public-site commands are reviewed illustrative recipes, not executed
external crawls. Ubuntu apt/macOS brew installation recipes were inspected against
build dependencies, not executed as package mutations. The core-only build mode
was already verified from an extracted source archive in Step 19.

No dashboard visual pass is inferred from a successful asset build or demo
process. Remote CI, Linux leak detection and real browser visual QA remain the
[tag gates](certification.md#ci-and-remaining-release-gates). Those checks were not
rerun or reclassified by documentation packaging.

## Changed file tree

```text
README.md                         revised project entrance
docs/
  README.md                       navigation and reading paths
  project-one-pager.md             concise architecture/evidence review
  design-walkthrough.md            one URL's ownership and state transitions
  interview-guide.md              30 questions, explanations, demo defense
  resume-bullets.md                bullets, claim map, GitHub/portfolio copy
  demo.md                          exact local crawl, recovery and demo steps
  release-notes-0.1.0.md           candidate scope and unexecuted gates
  packaging-verification.md       this record
```

Six meaningful documentation commits separate overview, design, interview,
resume/portfolio, demo and final navigation/release/verification. Build trees,
node_modules, dist assets, database/checkpoint companions, command logs and the
additional verification CSV remain untracked/ignored or outside the repository.
The selected Step 19 data/figures remain intentionally tracked evidence.

## Presentation decision

The project is suitable for a resume and technical portfolio with these scoped
claims and a reproducible local demo. It should be presented as a completed
single-process concurrency/performance experiment, with the candidate tag gates
stated honestly. Being ready to explain to recruiters does not imply that the
prepared release has passed unexecuted remote/platform/browser checks.
