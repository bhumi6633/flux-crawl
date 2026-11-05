# FluxCrawl

A C++20 network-concurrency and performance-engineering project that implements
the same crawling workload with threaded blocking I/O and event-driven
asynchronous I/O.

**Under development.** The project provides build infrastructure, version
information, a crawl CLI, an HTTP/HTTPS URL value type, and HTML anchor link
extraction, plus in-memory FIFO crawl state and global discovery deduplication.
The sequential, blocking thread-pool, and event-driven libcurl `curl_multi` engines
are implemented, with shared timing metrics and a reproducible local benchmark harness.
No general performance ranking is claimed.

## Build

Requires CMake 3.20 or newer, a C++20 compiler, and libcurl 7.85 or newer with
development headers, plus libxml2 development headers. On Debian/Ubuntu, install
`libcurl4-openssl-dev libxml2-dev`; the macOS Command Line Tools SDK includes both.
Python 3 is required when building tests (the default); no Python packages are
needed. Use `-DBUILD_TESTING=OFF` for a library/CLI build without Python.
URL operations remain local; HTTP fetches and crawler calls make requests.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For explicit configurations, use `-DCMAKE_BUILD_TYPE=Debug` or
`-DCMAKE_BUILD_TYPE=Release` at configure time, with separate build directories
such as `build-debug` and `build-release`. With multi-configuration generators,
select the configuration using `cmake --build build --config Debug` and
`ctest --test-dir build -C Debug --output-on-failure`.

## CLI

```bash
./build/fluxcrawl --help
./build/fluxcrawl --version
./build/fluxcrawl --depth 2 --max-pages 50 https://example.com
./build/fluxcrawl --engine threaded --workers 8 --depth 2 --max-pages 100 https://example.com
./build/fluxcrawl --engine async --concurrency 8 --depth 3 --max-pages 50 https://example.com
```

The version is defined in `include/fluxcrawl/version.hpp`. Help and version
requests exit successfully. Invalid options, invalid numeric values, or seed
arguments produce a diagnostic and status 1. A valid seed starts a sequential
crawl. `--depth N` defaults to 2, `--max-pages N` defaults to 100, and `--same-host`
optionally filters discoveries to the seed's exact normalized host. Zero values
are allowed. Arguments are checked before displaying help or version; help takes
precedence if both flags are present.

`--engine sequential` is the default. `--engine threaded` uses four workers unless
`--workers N` is supplied. Workers must be positive; supplying `--workers` with
any other engine is rejected. `--engine async` uses one event loop with up to eight
simultaneous transfers by default; `--concurrency N` changes that limit and must be
positive. Supplying it to sequential or threaded mode is rejected. Summaries identify
the engine and its worker/transfer limit. Ordinary crawl summaries retain their
existing fields; benchmark mode reports metrics.

The summary reports attempt/response/failure counts, HTML pages processed,
discoveries, and remaining queued URLs. Exit status is 1 if any fetch failed,
otherwise 0 (including a page-limit stop). HTTP 4xx/5xx responses alone do not
cause a nonzero exit. `--max-pages 0` validates the seed without making requests.
For a reproducible local demonstration that launches and cleans up the fixture:

```bash
python3 tests/http_server.py ./build/fluxcrawl --crawl-cli
python3 tests/http_server.py ./build/fluxcrawl --threaded-cli
python3 tests/http_server.py ./build/fluxcrawl --async-cli
```

## URL subsystem

`fluxcrawl::Url::parse(input)` and `Url::resolve(base, reference)` return
`std::optional<Url>` and report malformed or unsupported input with `std::nullopt`.
The public header exposes no curl types. Values have read-only component accessors;
`str()` and `canonical()` both return the normalized resource identity.
Equality and `std::hash<Url>` use that identity.

Normalization lowercases schemes and hosts, supplies `/` for an empty path,
removes fragments and default ports (HTTP 80, HTTPS 443), and removes literal
`.`/`..` path segments. Non-default ports, path case, repeated slashes, trailing
slashes, percent-encoding spelling, and query bytes/order are preserved.
`has_query()` distinguishes an empty `?` from no query. `port()` returns only an
explicit non-default port; port 0 is accepted syntactically.

Parsing uses the [libcurl URL API](https://curl.se/libcurl/c/curl_url_set.html),
with strict HTTP/HTTPS checks around its permissive input handling. Reference
resolution follows [RFC 3986 section 5.2](https://www.rfc-editor.org/rfc/rfc3986.html#section-5.2)
component replacement, path merging, and query inheritance; libcurl normalizes
the resulting path. Absolute URLs, network-path references (`//host/path`),
relative paths, empty references, queries, and fragments are supported.

This phase accepts ASCII URI text and valid percent escapes. Supply international
hostnames as ASCII A-labels (punycode), and encode non-ASCII path/query bytes.
Credentials, percent-encoded hosts, IPv6 zone identifiers, whitespace/control
characters, backslashes, malformed escapes, and empty ports are rejected.
IPv6 host accessors include brackets. No DNS lookup or host reachability check
is performed; host existence is not a parsing invariant. IDNA conversion and
additional deduplication policies remain future decisions.

## HTML link extraction

```cpp
#include <fluxcrawl/html_parser.hpp>

const auto base = fluxcrawl::Url::parse("https://example.com/docs/index.html");
if (base) {
    const auto links = fluxcrawl::HtmlLinkExtractor{}.extract_links(
        R"(<a href="../pricing">Pricing</a>)", *base);
    // links contains https://example.com/pricing
}
```

The stateless extractor parses bytes already in memory with libxml2's HTML parser
and selects `//a/@href` using XPath. It decodes HTML entities, trims surrounding
HTML whitespace, and delegates each reference to `Url::resolve`. Canonical URL
equality/hashing removes duplicates within this document while a vector preserves
first-seen document order. Empty hrefs and fragment-only links resolve to the
caller's base resource. Invalid and unsupported URLs are skipped.

Recoverable malformed HTML is accepted with parser diagnostics suppressed per
parse. Empty input, failure to build a document, and input exceeding libxml2's
`int` length limit return an empty vector. Normal libxml2 resource limits remain
enabled. Documents, XPath contexts/results, and decoded attribute strings use
RAII ownership; no process-wide cleanup or diagnostic hooks are changed.

The memory-only HTML API uses `HTML_PARSE_NONET`, does not load external DTDs or
entities, and performs no fetching. The caller's base URL is authoritative:
`<base>` and non-anchor resource attributes are ignored. Encoding detection is
left to libxml2 (including document declarations); HTTP charset metadata is not
part of this API yet. This is libxml2 HTML recovery, not a browser HTML5 DOM.
There is no same-domain filtering, crawler-wide deduplication, or scheduling.

## Crawl state

`CrawlItem` holds a `Url` and a `std::size_t depth`; it lives alongside the small
`CrawlFrontier` API in `crawl_frontier.hpp`. The frontier uses a deque with
`push(CrawlItem)`, `pop()`, `size()`, and `empty()`. Empty pops return `std::nullopt`.
The standalone frontier applies no deduplication or depth policy.

`CrawlState(Url seed, std::size_t max_depth)` owns a frontier, an exact
`std::unordered_set<Url>` of seen resources, and an inclusive depth limit. Its API:

- `next()`: remove the oldest queued item, or return `std::nullopt`.
- `discover(url, depth)`: accept new work within the depth limit; return `false`
  for duplicates or excessive depth without changing state.
- `discover_from(parent, url)`: discover at `parent.depth + 1`, checking the limit
  before addition to avoid overflow.
- `has_seen(url)`, `seen_count()`, `frontier_size()`, and `empty()`: inspect state.

The seed is automatically queued and marked seen at depth 0. Its children have
depth 1, and their children depth 2. A maximum of 2 permits depths 0 through 2;
a maximum of 0 permits only seed-depth work. Rejected over-depth URLs are not
marked seen and may be accepted later at a valid depth.

Marking URLs seen at acceptance prevents two pages from queueing the same pending
resource. Canonical URL equality/hashing governs duplicates across the entire
state, including after an item is popped. First discovery wins: rediscovery does
not requeue work or revise its depth. Enqueue allocation failures roll back the
seen insertion before propagating the exception.

FIFO gives deterministic BFS when callers expand items in queue order. For
`A -> [B,C,D]`, `B -> [E,F]`, and `C -> [G]`, processing order is
`A,B,C,D,E,F,G`. Explicit depths and parent items are supplied by the caller;
the state does not validate parent provenance or enforce shortest-path updates.
Parser output composes by passing each extracted `Url` to `discover_from`; the
state has no dependency on the parser and no same-domain restriction.

These classes are single-threaded. `empty()` means the frontier has no queued
items, not that popped work has finished. The threaded engine wraps them in its
own coordinator to account for in-flight work and synchronization.
The crawl state includes no networking, completed-request tracking, or production
crawl loop.

## Blocking HTTP client

```cpp
#include <fluxcrawl/http_client.hpp>

const auto url = fluxcrawl::Url::parse("https://example.com/");
if (url) {
    const fluxcrawl::HttpClient client;
    const fluxcrawl::HttpResult result = client.fetch(*url);
    if (const auto* response = std::get_if<fluxcrawl::HttpResponse>(&result)) {
        // Inspect status_code, content_type, body, and effective_url.
        // Future HTML processing should use effective_url as its base.
    } else {
        const auto& error = std::get<fluxcrawl::HttpError>(result);
        // Inspect error.code, error.curl_code, and error.message.
        (void)error;
    }
}
```

`HttpClient(HttpClientOptions = {})` validates options and initializes libcurl.
`fetch(const Url&) const` performs one blocking logical GET (including redirects),
without retrying. It does not parse HTML or access crawl state. The sequential
crawler and CLI compose it with those separate components.

`HttpResponse` contains requested and normalized effective URLs, the numeric HTTP
status, the supplied Content-Type (empty when absent), a binary-safe `std::string`
body, and `downloaded_bytes`. The count is the decoded body size, not compressed
wire traffic. Explicit byte counts preserve embedded NULs and append all chunks.
404/429/5xx responses are responses, not transport failures.

`HttpResult` is `std::variant<HttpResponse, HttpError>`. Error categories distinguish
libcurl failures, body-limit violations, callback buffer failures, invalid effective
URLs, and missing HTTP status. Errors retain a numeric curl code where applicable
and a diagnostic message; incomplete bodies are not exposed as successful responses.
Routine transport failures do not throw. Invalid construction options/global
initialization failures may throw, as may ordinary C++ allocation failures outside
the callback. No exception escapes the C write callback.

Options default to a 5-second connection timeout, 30-second overall timeout,
10 redirects, 8 MiB maximum decoded body, and `FluxCrawl/` plus the project version.
Timeouts must be positive and fit libcurl's millisecond `long`; redirects must be
nonnegative. A zero body limit accepts only empty bodies. The callback checks the
limit before appending, including decompressed data. Compression support is supplied
by libcurl's build through `CURLOPT_ACCEPT_ENCODING`.

Initial and redirected transfers are restricted to HTTP/HTTPS using libcurl's
[protocol options](https://curl.se/libcurl/c/CURLOPT_PROTOCOLS_STR.html), which
require version 7.85. Redirect loops are bounded; a limit of zero rejects following
a redirect. Effective URLs must pass `Url::parse`. TLS peer and hostname verification
retain secure defaults. No cookies, credentials, or retry policy are configured.
Libcurl's standard environment proxy behavior applies outside the isolated tests.

A function-local static RAII object performs global initialization once and cleanup
at process shutdown. Each fetch owns a fresh easy handle and callback buffer, so
requests share no easy handle or mutable response storage. Clients/workers must
finish before process shutdown. Blocking fetches do not reuse a connection pool
across calls; the async multi handle can reuse connections within its crawl.
`CURLOPT_NOSIGNAL` is set for independent worker requests. DNS timeout behavior
depends on libcurl's resolver backend; an asynchronous or threaded resolver build
is needed to bound name lookup with signals disabled.

The test fixture runs a threaded Python HTTP server on `127.0.0.1` with an ephemeral
port, starts the C++ test as a child process, clears proxy environment variables,
and always reaps the child and closes sockets. A reserved non-listening socket
provides a deterministic unavailable endpoint (refusal or timeout depends on the OS).
No public internet services are contacted. Tests cover GET, HTML metadata, redirects,
404/503, binary/chunked/gzip bodies, missing Content-Type, empty responses, exact/zero/
exceeded body limits, connection failure, request timeout, redirect loops and protocol
restrictions, invalid effective URLs, truncated bodies, user agents, and option
validation. TLS verification is preserved but a local TLS fixture is not included.

## Sequential crawler

`SequentialCrawler(HttpClientOptions = {})` owns a configured blocking client.
`crawl(const Url& seed, const CrawlOptions& = {}) const` creates fresh per-crawl
state. `CrawlOptions` contains `max_depth = 2`, `max_pages = 100`, and
`same_host_only = false`. There is exactly one active transfer at a time.

```text
Seed -> CrawlState -> SequentialCrawler -> HttpClient -> HttpResponse
            ^               |                               |
            |               +-- successful HTML ------------+
            |                           |
            +-- discovered URLs <- HtmlLinkExtractor
```

Each iteration claims eligible host-scheduled work, records a fetch attempt,
and processes the fetch result. Only 2xx responses with the exact case-insensitive media type `text/html`
(ignoring surrounding whitespace and parameters) are extracted. At maximum depth,
the response is recorded without extracting links. Other statuses, missing types,
and non-HTML types never produce discoveries. Extraction uses the effective URL
after redirects. Fetch errors are recorded and the remaining queue continues;
retries are optional and disabled by default (see below). Malformed HTML uses the existing parser's recovery behavior.

All discoveries go through `CrawlState::discover_from`; no second seen set exists.
Depth 0 is the seed, and limits are inclusive. Depth barriers preserve BFS;
within a depth, hosts rotate fairly and each host retains discovery order.
`max_pages` counts request attempts, including retries, failures and HTTP errors;
redirect hops are part of the same attempt. Zero pages performs no fetches, leaving
the seed discovered and pending. The final permitted response is still processed,
so it can leave discoveries queued when the page budget is exhausted.

`CrawlResult` contains `pages_attempted`, `responses_received`, `fetch_failures`,
`html_pages_processed`, `urls_discovered`, `urls_pending`, and ordered `records`.
Discovered means accepted into crawl state, including the seed; attempted means
`fetch` was invoked; responded means an `HttpResponse` was returned. Fetch failures
include all `HttpError` categories, including client limits and invalid metadata.
HTML processed counts actual extraction calls, excluding depth-boundary pages.
Each record stores its `CrawlItem` and either response metadata (effective URL,
status, content type, decoded bytes) or the original `HttpError`. Bodies are released
after each iteration, not retained in the result.

Same-host policy compares normalized hosts exactly, ignores ports and schemes,
and filters discoveries only. It does not restrict HTTP redirects. Redirect aliases
remain separate requested identities: reaching a final URL by redirect does not
mark that final URL separately seen, so it can be fetched again if independently
discovered. Page limits bound attempts, not the size of the discovered frontier.
These semantics are the reference behavior shared by all three engines.
Host concurrency caps, token-bucket rate limiting, and delayed retries are optional.
Robots.txt and persistence are not implemented.

The loopback crawl graph has `/crawl/ -> [a,b,shared,redirect-start]`,
`a -> [deep,shared]`, `b -> [shared,/crawl/]`, and
`redirect-start -> redirected/index.html -> child.html`. Tests assert BFS order,
deduplication, zero/inclusive limits, failure accounting and continuation, 404/503
handling, malformed HTML recovery, MIME gating, redirect bases, and host filtering.
The CLI fixture also asserts the physical server request sequence.

## Blocking thread-pool crawler

```cpp
#include <fluxcrawl/thread_pool_crawler.hpp>

// Construct before starting any external workers that might use these libraries.
const fluxcrawl::ThreadPoolCrawler crawler{4};
// crawler.crawl(seed, options) returns the same CrawlResult model as sequential.
```

`ThreadPoolCrawler(std::size_t workers = 4, HttpClientOptions = {})` rejects zero
workers. `crawl(const Url&, const CrawlOptions& = {}) const` creates a fixed pool
for this call, joins it, and returns. Zero page budget returns without starting
workers. The shared options/results now live in `crawl_types.hpp`; existing
sequential includes remain compatible.

```text
                 Coordinator: CrawlState + results
                  mutex / condition variable
                      /        |        \
                 Worker 1   Worker 2   Worker N
                      \        |        /
                  independent blocking fetches
                  worker-local HTML documents
                             |
                   publish links and metadata
```

The private coordinator has one mutex protecting discovery and host queues,
seen set, host/global active counts, token buckets, attempt count, fatal error,
and result aggregation. Claiming an item reserves its attempt budget and increments active
work in the same critical section. Fetching and parsing run outside the lock.
Publishing performs the canonical seen check, insertion, enqueue, and result
update under the lock, then decrements active work and notifies all waiters.

Workers recheck eligibility after notifications, spurious wakeups, and the earliest
token or retry deadline using `condition_variable::wait_until`, which releases the mutex.
A worker exits
normally only when the budget is spent or no queued/active work remains. Temporary
frontier emptiness never implies termination while another worker can discover
links. Existing claims finish even after the budget is reached; all their results
and discoveries are published. There is no busy waiting or detached work.

Workers run concurrently within each depth level; the next level waits for the
current active level to finish. This small depth barrier preserves shortest first
discovery and the sequential reachable set under depth limits: otherwise a fast
long path can mark a URL seen too deeply before a slow shorter path arrives.
It does not impose completion ordering within a level. Round-robin acquisition,
publication, and record order are scheduling-dependent; threaded records use publication order.
With a truncating page limit, the particular attempted subset can differ from
sequential even though the exact budget is respected. Without truncation, the
engines agree on requested URL/depth sets for a stable graph.

Unexpected worker exceptions stop new claims, decrement any claimed active work,
and wake every waiter. Partially started pools are also stopped and joined. The
first exception is rethrown on the calling thread after joining; no partial result
is presented as success. Already-running blocking requests finish or reach their
configured timeouts. Routine HttpError outcomes remain nonfatal crawl results.

The immutable HttpClient configuration is shared, but each fetch owns its easy
handle and buffers. Global curl initialization happens during crawler construction,
before pool startup. This follows [libcurl's threading requirements](https://curl.se/libcurl/c/threadsafe.html).
Libxml2 initialization runs once before workers start; each extraction has independent
document/XPath objects. No per-thread or per-request global cleanup is performed.
Use thread-enabled libxml2 and thread-safe libcurl dependencies.

Tests compare sequential and threaded URL/depth sets and response metadata, exercise
1/2/4/8/16 workers, empty expansion, temporary emptiness, failure continuation,
redirects, MIME/host filtering, and a deliberately slower shortest path. Each run
repeats duplicate/termination and 16-worker five-attempt races 25 times. A 102-page
generated graph is checked with 1/2/4/8 workers. Synchronized server counters assert
one worker has peak concurrency 1 and four workers overlap; they also verify the
shared target receives exactly one request. There are no timing speedup assertions.

Local verification used AppleClang 16: all tests passed under ASan/UBSan, and the
threaded engine/CLI passed TSan without race reports. Linked system libraries are
not rebuilt with sanitizers, so this is evidence for the exercised paths rather
than a proof of all possible schedules.

## Event-driven async crawler

```cpp
#include <fluxcrawl/async_crawler.hpp>

const fluxcrawl::AsyncCrawler crawler{8};
// crawler.crawl(seed, options) returns the shared CrawlResult.
```

`AsyncCrawler(std::size_t concurrency = 8, HttpClientOptions = {})` rejects zero
concurrency and validates the same HTTP options as the blocking client.
`crawl(const Url&, CrawlOptions = {}) const` owns fresh state and one multi handle
per call. All networking coordination, callbacks, HTML extraction, discovery, and
result publication run on the calling thread. FluxCrawl starts no async worker
threads and uses no coroutines, mutexes, or condition variables in this engine.
Libcurl/system resolver internals may use threads.

```text
                       Shared crawl semantics
                       /         |           \
                Sequential    Thread pool    Async multi
                1 blocking    N blocking     1 event-loop thread
                request       workers        N active transfers
                       \         |           /
                  Shared HTTP setup and response processing
                       HTML extraction + CrawlState
```

The loop fills eligible host-scheduled slots, calls `curl_multi_perform`, drains
every
`CURLMSG_DONE` from `curl_multi_info_read`, processes responses, and refills slots
before waiting. Newly added handles get a perform call before the next wait.
[`curl_multi_poll`](https://curl.se/libcurl/c/curl_multi_poll.html) waits up to
1000 ms, shortened by the next host token deadline and internal libcurl timers.
Unlike `curl_multi_wait`, it
also sleeps when there are no descriptors, so zero descriptors are normal and do
not cause a busy loop or false failure. It is supported by the existing libcurl
7.85 minimum.

Concurrency bounds active logical transfers, including redirects, rather than
threads or fixed batches. Completion frees a slot immediately for eligible
host-scheduled work, even while other transfers remain active. As with the threaded engine,
advancing to the next depth waits for the current depth to finish. This depth
barrier preserves shortest first discovery under inclusive depth limits; it does
not batch same-depth transfers. At a level boundary some capacity can be idle.
Record order follows completion processing, and a truncated crawl can select a
different subset than sequential. No completion ordering is forced.

An internal `unordered_map<CURL*, unique_ptr<ActiveTransfer>>` keys each handle to
its item and shared `CurlTransfer`. Heap ownership keeps callback body/error
buffers at stable addresses through map growth. The transfer is noncopyable and
nonmovable. Its easy handle is destroyed before its buffers. Both HTTP engines
use the same setup, write callback, decoded-body cap, and result conversion in
`src/curl_transfer.*`; blocking `HttpClient::fetch` remains a blocking easy call.

Map insertion precedes attachment, avoiding a stranded handle on allocation
failure. On completion the code copies the message result before removing the
easy handle, then extracts metadata, processes links, and destroys the transfer.
[`curl_multi_info_read` message storage](https://curl.se/libcurl/c/curl_multi_info_read.html)
must not be used after removal. Normal and exception cleanup detach remaining
easy handles before freeing them, then clean up the multi handle, following
[libcurl's ownership order](https://curl.se/libcurl/c/curl_multi_cleanup.html).
Global cleanup occurs only at process shutdown. Multi init/add/perform/poll/remove
and normal cleanup errors throw as fatal engine errors; unwinding cleanup cannot
throw. No partial crawl result is returned on a fatal error.

An attempt is reserved when a frontier item is committed to transfer setup; even
an easy setup failure consumes that attempt, matching blocking behavior. Filling
requires both `active < concurrency` and `attempted < max_pages`. Existing active
transfers finish and publish their discoveries after the budget is exhausted.
Zero pages performs no transfers. An empty frontier with active work keeps waiting;
termination requires no active transfers and either no pending work or no budget.
Pending counts include host queues, rate-limited work, and delayed retries. With no
active transfers, the loop sleeps until the next eligibility deadline and resumes.
`CrawlState` handles canonical deduplication and depth acceptance; `HostScheduler`
handles claims, fairness, depth barriers, and per-origin eligibility.

Individual HTTP/client failures become records and allow siblings to continue.
404/500/503/429 remain responses. Oversized decoded bodies abort only that transfer;
partial bodies are never parsed. Redirects, redirect limits, timeout values,
User-Agent, compression, secure TLS defaults, media-type gating, and effective-URL
link bases use the shared HTTP and processing code. Redirect aliases retain the
same requested-identity semantics as the other engines.

The async test suite compares URL/depth sets, counters, full response metadata,
and error categories against both reference engines. It exercises concurrency
1/2/4/8/32/64, exact and zero budgets, depth limits, host filtering, redirect bases,
HTTP failures, transport failures, timeouts, binary/gzip/chunked bodies, malformed
HTML, and surplus capacity. Server counters verify overlap and concurrency caps.
A controlled refill graph holds A and C until D starts after B completes: ten
repetitions prove replacement happens before the other requests finish. Ten
repetitions also cover temporary empty frontier expansion, exact five-request
budgets confirmed by server counts, oversized-body isolation, and transport-error
isolation. High fanout visits 102 unique pages with concurrency 1/2/4/8/16, checking
server counts and caps. Tests never assert a speedup.

For sanitizer verification:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitize
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure
```

Step 7 local verification on AppleClang 16 passed all 41 CTest tests in both normal
and ASan/UBSan builds. The async executable reported 2,230 checks with no failures,
including server peaks of 1, 3, and 4 at those configured limits. Running that
executable through macOS `leaks --atExit --` under the same local fixture reported
zero leaks (zero leaked bytes). Each engine's CLI demonstration reported seven
attempts, seven responses, seven HTML pages, zero failures, seven discoveries, and
zero pending URLs. System dependencies are not sanitizer-instrumented; fatal
multi errors and allocator exhaustion were reviewed but are not fault-injected.

For future measurements, account for depth barriers in both concurrent engines,
HTML parsing on the async event-loop thread versus worker-local parsing, and
libcurl's multi connection reuse versus fresh blocking easy handles. Concurrency
is a transfer cap, not a rate limit or necessarily a socket count (redirects and
protocol multiplexing matter). Decoded bodies can occupy up to roughly
`concurrency * max_body_bytes` plus parser/library storage; discovered URLs and
metadata records also consume memory and are not bounded by the active cap.
The local fixture uses HTTP/1.0 close-after-response behavior, so it does not test
connection reuse or TLS. Resolver, TLS, HTTP version, and connection policy need
explicit control in later experiments. No engine performance ranking is claimed.

## Metrics and reproducible benchmarks

FluxCrawl does not assume that threads or async I/O are faster. It runs the same
deterministic crawl workload through each engine and records the resulting
behavior. Engine scheduling and connection management are unchanged in this phase.

```text
                    Deterministic workload
                     /        |        \
                Sequential Threaded    Async
                     \        |        /
                       Shared metrics
                             |
                       BenchmarkReport
                         /         \
                       CLI         CSV
```

`CrawlRecord::latency` and `CrawlResult::elapsed` use `steady_clock` durations stored
as nanoseconds. Sequential/threaded request latency starts immediately before
`HttpClient::fetch` and ends when it returns. Async latency starts immediately
before adding the configured easy handle and ends when the event loop observes
its `CURLMSG_DONE`. Easy setup failures record their setup duration. Both include
redirects and failed attempts, exclude frontier queue time, and stop before that
response's HTML extraction. Blocking latency includes easy setup/result conversion
and cleanup; async timing includes event-loop observation delay (including work on
previous completions) and excludes easy configuration/metadata conversion. These
are client-observed active durations, not isolated server or wire latency.

Crawl elapsed covers the crawl call from state initialization through result
aggregation, including request waiting, parsing, scheduling, worker startup/join,
and async multi cleanup. Engine construction and benchmark control requests are
outside that interval; final local-container destruction on return is also outside.
`bytes_downloaded` sums decoded retained bodies from all `HttpResponse` outcomes,
including HTTP error statuses. It excludes headers, compressed wire size, redirect
intermediate bodies, and partial bodies discarded on client failures. Existing
`fetch_failures` counts all `HttpError` categories, so CSV does not relabel it as
transport-only failures. Records retain metadata and durations, never bodies.

`crawl_metrics(result)` derives:

- `attempts_per_second = pages_attempted / elapsed_seconds` (primary rate).
- `responses_per_second = responses_received / elapsed_seconds`.
- `bytes_per_second = bytes_downloaded / elapsed_seconds`.
- Mean, minimum, maximum, and P50/P95/P99 over recorded attempts, including failures.
  Percentiles use nearest rank:
  sort N observations, then select index `ceil(p * N / 100) - 1`.

Empty observations yield unavailable latency statistics. Mean latency uses a
wide floating-point sum to avoid integer-duration sum overflow. Nonpositive elapsed time yields
unavailable rates; zero attempts with positive elapsed yields zero rates. CSV uses
empty fields for unavailable values; the table uses `NA`. Tests cover empty,
single, two, five, and 1,000-observation sets, large durations, negative-input
rejection, and known rate arithmetic.

Build Release for measurements; sanitizer and Debug runs are correctness checks:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Start the server in one terminal:

```bash
python3 tools/benchmark_server.py --port 8080 \
  --pages 50 --fanout 4 --delay-ms 20 --body-bytes 1024
```

It prints its seed URL. Port 0 selects an available port. In another terminal:

```bash
./build-release/fluxcrawl benchmark \
  --seed http://127.0.0.1:8080/page/0 \
  --engines sequential,threaded,async --concurrency 1,2,4 \
  --depth 20 --max-pages 50 --runs 2 --warmups 1 \
  --output benchmark_results.csv
```

Stop the server with Ctrl-C. The production binary never starts Python. The CLI
exclusively creates a new CSV file and rejects existing or unwritable paths before
running trials. An unsuccessful run removes its newly created output. Omit
`--output` for table-only output. No report or CSV rows are emitted until all trials
validate. `fluxcrawl benchmark --help` lists defaults and options. Build type and
FluxCrawl version are recorded; a non-Release build prints a notice but can run.

Five named scenarios are supplied by `tools/benchmark_server.py`:

| Scenario | Pages | Fanout | Delay (ms) | Body (bytes) |
|---|---:|---:|---:|---:|
| `fast-small` | 100 | 4 | 2 | 1024 |
| `latency-bound` | 100 | 4 | 50 | 1024 |
| `higher-latency` | 100 | 4 | 150 | 1024 |
| `high-fanout` | 100 | 99 | 5 | 4096 |
| `large-body` | 100 | 4 | 10 | 65536 |

`BenchmarkScenario` is a small immutable Python preset model. The C++
`BenchmarkWorkload` carries the reported scenario name and actual parameters.
The first three vary waiting time; high fanout exposes all 99 children after the
seed; large bodies add copying and parsing work. Graphs contain no randomness.
Server overrides (`--pages`, `--fanout`, `--delay-ms`, `--body-bytes`) take precedence
over preset values, and the report always records the actual parameters alongside
the preset name. An undersized body still fails validation.

For a named scenario and unequal sweeps, start the server separately:

```bash
python3 tools/benchmark_server.py --scenario latency-bound --port 8080 \
  --pages 20 --delay-ms 50
```

Then run the comparison (the server is ready when it prints its URL):

```bash
./build-release/fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario latency-bound --engine all --workers 1,2,4,8 \
  --concurrency 1,2,4,8,16 --max-pages 20 --runs 2 \
  --output benchmark_results.csv
```

Client `--scenario` checks the server's reported name; it does not reconfigure a
running server. A mismatch fails before any measured trial. Without a preset the
server reports `custom` and retains the original modest defaults. Protocol v2
adds the scenario name; the client also accepts v1 metadata as `custom`.

The generated site is a connected tree: page i links to children `fanout*i+1`
through `fanout*i+fanout`, stopping before `pages`. IDs are `/page/0`, `/page/1`, etc.
Every response is HTTP 200 `text/html`, padded with spaces to exactly `body-bytes`.
Too-small bodies are rejected, rather than silently changing payload sizes. The
server supports 1..100,000 pages, fanout 1..1,000, delay 0..10,000 ms, and bodies up
to 1 MiB. Artificial delay represents server/network waiting without internet
variance. Defaults are modest: 50 pages, fanout 4, delay 20 ms, body 1 KiB.

The concurrent server shares the correctness fixture's `ThreadingHTTPServer` base,
with a connection backlog of 128. It counts total requests, unique page IDs,
current work, and peak work under a lock. Server work ends before response dispatch
so completed-handler teardown cannot overcount replacement requests. Control
endpoints `/__benchmark/config`, `/__benchmark/reset`, and `/__benchmark/stats`
are excluded from those counters and client metrics. Reset rejects outstanding
work; the runner checks idle state and request totals after every trial. Use a
dedicated server per benchmark invocation; concurrent benchmark drivers are not
supported. No traffic from other clients should share it.

`BenchmarkConfig` selects seed, crawl limits, engines, concurrency levels, measured
runs, and optional warmup sweeps. `BenchmarkRunner::run` returns a `BenchmarkReport`
with compact `BenchmarkSample` rows, workload metadata, and per-configuration
medians. Each trial constructs fresh engine and crawl state. Duplicate engines or
levels are deduplicated; ordering is repetition-major, then sequential, threaded
in increasing concurrency, and async in increasing concurrency. Sequential runs
once per repetition at concurrency 1. For threaded, concurrency counts workers;
for async, it caps simultaneous transfers. `--engine` aliases `--engines`, and
`all` selects every engine. `--workers LIST` overrides only the threaded sweep;
without it, threaded mode uses the shared `--concurrency` list as before. Explicit
workers require the threaded engine to be selected. Library callers use
`BenchmarkConfig::workers`; an empty vector means the shared-list fallback.
Benchmarks reject more than 256 workers, 1,024 async transfers, or 10,000 runs or
warmup sweeps. These guards prevent obvious accidental resource exhaustion and do
not change crawler APIs. Generated files can be placed in the ignored `results/`
directory (create it before export). Warmup sweeps use every configuration
and are validated but never included in samples or medians. Connections are not
kept across independent trials.

The runner accepts only the supplied server protocol at a literal
`http://127.0.0.1:<port>/page/0` seed. All engines bypass environment proxies, disable
redirect following, and filter discoveries to the seed host during benchmarks.
The controlled workload has no redirects, and these settings are identical for
all three engines. Ordinary crawler proxy/redirect defaults remain unchanged.
The server client module handles control traffic; the runner delegates all crawl
networking to the existing engines.

Expected work is computed from the server's page count/fanout and the selected
depth. Every accepted trial must have the exact expected requested URL/depth set,
no duplicates or fetch failures, HTTP 200 HTML metadata, the exact body-byte total,
no pending URLs, and matching server total/unique counts with peak work at or below
configured concurrency. A lower depth intentionally benchmarks its known reachable
subset. A nonzero page budget smaller than that subset is rejected: truncating
completion-dependent frontiers can select different workloads across engines.
Zero pages is supported as an empty measurement with zero requests. Missing links,
wrong bodies, or engine mismatches invalidate the run instead of appearing as wins.

CSV uses stable columns, a classic numeric locale, decimal milliseconds with six
fractional digits, raw byte counts, and quoted/escaped string metadata. It records
version, build, seed, workload parameters, limits, expected count, warmups, engine,
configured concurrency, run, counters, elapsed, the three rates, latency sample
count/P50/P95/P99, and explicitly labeled server observations. New trailing columns
are `scenario`, `latency_mean_ms`, `latency_min_ms`, `latency_max_ms`, `measured_runs`,
and `started_at_unix_ms`. The timestamp is UTC Unix milliseconds captured outside
the crawl timer; all duration measurements still use `steady_clock`. Each measured trial
has one row. The console also shows median elapsed and median attempts/sec across
runs per configuration (arithmetic midpoint for even counts); unavailable rates
are excluded from rate medians. Percentiles remain per-request within each trial,
not percentiles of per-run percentiles. Console aggregates also report population
standard deviation of elapsed milliseconds across measured trials (divisor N,
Welford accumulation; one trial has deviation zero).

The test suite checks graph connectivity and exact payload size, arithmetic,
percentiles, median calculations, CSV quoting/locale/empty values, three-engine
samples, 12-row repetition sweeps, warmup exclusion, zero work, depth subsets,
invalid arguments/paths, existing-file preservation, proxy bypass, and deliberate
missing-link rejection, all five scenarios with tiny overrides, mean/extrema,
variance, unequal sweeps, scenario mismatch, and resource limits. CI makes no
throughput or speedup assertions.

Step 8 verification on AppleClang 16 passed all 45 tests in normal and ASan/UBSan
builds. TSan passed the threaded crawler, threaded CLI, and benchmark integration
tests without reports. The named-scenario Release smoke run used `latency-bound`
with 20 pages, fanout 4, 50 ms delay, and 1 KiB bodies. It ran sequential once per
repetition, threaded at 1/2/4/8, and async at 1/2/4/8/16, with two measured runs and
one excluded warmup sweep. All 20 CSV rows validated 20 responses, 20,480 decoded
bytes, zero failures, numeric fields, and distinct run/configuration identities.
The generated CSV stays under ignored `results/`. This verifies the harness;
it does not establish a general performance ranking.

Interpret measurements with the environment in mind: client and Python server
compete for resources on one machine; Python thread creation, the GIL, accept
backlog, and body generation can dominate at high concurrency or zero delay. The
server uses HTTP/1.0 with connection close, without TLS or compression, so this
workload does not exercise persistent connection reuse. Native libcurl reuse
behavior remains unchanged in the engines. Depth barriers, async parsing on the
event loop, resolver choice, build options, and OS scheduling affect results.
CPU time, RSS, and context switches are deferred to a later profiling module.
This harness establishes reproducible observations; it does not establish a
general engine ranking or include optimization, retries, rate limiting, or persistence.

## Layout

- `include/fluxcrawl/`: public version, URL, and HTML extractor headers.
- `src/url.cpp`: URL implementation in the compiled `fluxcrawl_core` library.
- `src/html_parser.cpp`: libxml2 HTML extraction, also in `fluxcrawl_core`.
- `include/fluxcrawl/http_client.hpp`, `http_response.hpp`, and `src/http_client.cpp`:
  blocking HTTP API and implementation.
- `include/fluxcrawl/crawl_frontier.hpp` and `crawl_state.hpp`: public crawl work
  and state APIs, implemented by the corresponding `.cpp` files in `src/`.
- `src/main.cpp`: crawl entrypoint; `src/benchmark_cli.*`: benchmark argument/output handling.
- `include/fluxcrawl/metrics.hpp`, `src/metrics.cpp`: portable rates and latency percentiles.
- `include/fluxcrawl/benchmark.hpp`, `src/benchmark*.cpp`: runner, reports, control client, and CSV/table output.
- `tools/benchmark_server.py`: deterministic benchmark site; `tools/local_http.py`: shared server base.
- `include/fluxcrawl/sequential_crawler.hpp` and `src/sequential_crawler.cpp`:
  sequential orchestration.
- `include/fluxcrawl/crawl_types.hpp`: shared options and metadata-only results.
- `include/fluxcrawl/thread_pool_crawler.hpp` and `src/thread_pool_crawler.cpp`:
  fixed worker pool and private crawl coordinator.
- `include/fluxcrawl/async_crawler.hpp` and `src/async_crawler.cpp`: single-threaded
  multi event loop and transfer ownership.
- `src/curl_transfer.hpp` and `.cpp`: internal shared easy setup, buffers, and results.
- `src/crawl_processing.hpp` and `.cpp`: shared fetch/parse/publication rules.
- `src/http_response.cpp`: reusable HTML media-type check.
- `cmake/`: target-scoped compiler warning configuration.
- `tests/`: offline CTest CLI, version, URL, and HTML extraction checks, without
  a test framework. HTML tests block external resource loading with a test-only
  guard and exercise malformed markup, entities, ordering, and deduplication.
  Crawl-state tests cover FIFO/BFS, depth boundaries, discovery-time deduplication,
  and composition with in-memory parser results.

Existing local `lab4/` and `lab5/` coursework is separate from FluxCrawl and is
not included in its build.

Released under the [MIT license](LICENSE).

## Profiling and scaling analysis

Benchmarking tells you how fast; optional profiling helps explain where time
is spent. After starting the loopback benchmark server, use:

```sh
build-release/fluxcrawl benchmark --seed http://127.0.0.1:8080/page/0 \
  --scenario latency-bound --engine all --concurrency 1,4,8,16 \
  --runs 3 --profile --output results/profile.csv
```

Run the same sweep without `--profile` for headline throughput. Profiles include
HTML extraction/URL resolution, global discovery, worker lock and idle waits,
and async event-loop behavior. Categories overlap and do not measure CPU
utilization. New `link-heavy` and `mixed-latency` scenarios expose processing
costs and out-of-order completions. See [profiling definitions and external-tool
commands](docs/profiling.md) and the [measured analysis](profiling-report.md).

## Evidence-based optimization

The first measured optimization avoids resolving identical decoded hrefs more
than once per document, while retaining canonical URL deduplication and discovery
order. On the documented Apple M3 link-heavy workload, five-run median throughput
improved about 54–68% across the tested configurations; reversed-order trials
confirmed the benefit. Network and low-duplicate workloads showed no meaningful
improvement. The cache adds per-document storage, and measured memory accounting
was mixed. See the [optimization report](docs/optimization-report.md) for full
before/after latency, variance, profiler evidence, memory tradeoffs and reproduction.

## Host-aware scheduling and politeness

All three engines use a shared origin scheduler with round-robin fairness,
per-origin concurrency caps and optional token-bucket rate limiting:

```sh
./build/fluxcrawl --engine async --concurrency 32 --max-per-host 4 \
  --rate-per-host 10 --burst-per-host 1 --depth 3 --max-pages 500 https://example.com
```

Global concurrency limits total active work. `--max-per-host` limits active claims
for one normalized scheme/host/effective-port origin. `--rate-per-host` controls
admission frequency, and `--burst-per-host` controls initial/accumulated tokens.
Caps default to unlimited relative to global concurrency; rate limiting defaults
to disabled and configured burst defaults to one. Fairness applies among eligible
hosts at the current breadth-first depth, without promising completion order.

Redirect hops remain charged to the original requested origin; this is not a
per-hop rate guarantee. `--same-host` remains hostname-only discovery filtering.
The original benchmark runner requires disabled politeness limits. An optional
multi-origin scenario records its limits separately. See
[host scheduling architecture, tests and reproduction](docs/host-scheduling.md).


## Retries and delayed work

All engines support optional retries through `CrawlOptions::retry` or the CLI:

```sh
./build/fluxcrawl --engine async --concurrency 8 --max-retries 2 \
  --retry-backoff-ms 250 --retry-max-backoff-ms 5000 --retry-jitter \
  --max-per-host 2 --rate-per-host 10 --max-pages 100 https://example.com
```

The default `--max-retries 0` preserves previous crawl/benchmark behavior. Two
retries permit one initial attempt plus two retries. Initial backoff defaults to
250 ms, exponential multiplier to two, cap to 5000 ms, and full jitter to enabled.
`--no-retry-jitter` makes delays deterministic. Positive delays and cap >= initial
are required; the library also rejects nonfinite or less-than-one multipliers.

Retryable HTTP statuses are **429, 500, 502, 503, 504**. Selected curl timeout,
host-resolution, connection, receive/send, empty-reply and partial-file failures
also retry. Other HTTP statuses, TLS/configuration failures, body limits, invalid
metadata, non-HTML content and malformed HTML do not trigger retries.

Retry `n` waits a uniform full-jitter delay from zero through the capped value
`min(max_backoff, initial_backoff * multiplier^(n-1))`, rounded down to milliseconds.
Final-response integer-seconds `Retry-After` is a lower bound on that delay and
may exceed the backoff cap. Malformed/overflow/date-form headers fall back to the
normal policy; HTTP-date support remains future work.

Retries bypass discovery deduplication and retain original URL/depth, then
re-enter ordinary host scheduling when due. They must satisfy host/global
concurrency and token-bucket limits. Other eligible work continues during backoff.
Existing breadth-first barriers also include delayed retries: deeper work can wait
for a retry at a shallower depth. Every retry consumes `max_pages`; delayed work
keeps a crawl alive until it runs or the page budget stops the crawl.

Summaries distinguish scheduled/attempted/exhausted/cancelled retries, recovered
resources, terminal HTTP/transport failures, and attempt-level responses/errors.
A recovered transient transport failure remains visible in attempt records but
returns CLI success. With retries enabled, terminal HTTP failures also produce a
nonzero exit status. Baseline benchmarks reject enabled retries.

See [retry architecture, accounting, tests and verification](docs/retries.md).
