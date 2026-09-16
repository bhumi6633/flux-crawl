# FluxCrawl

A C++20 network-concurrency and performance-engineering project that implements
the same crawling workload with threaded blocking I/O and event-driven
asynchronous I/O.

**Under development.** The project provides build infrastructure, version
information, a crawl CLI, an HTTP/HTTPS URL value type, and HTML anchor link
extraction, plus in-memory FIFO crawl state and global discovery deduplication.
The sequential and blocking thread-pool engines are implemented. The event-driven
libcurl `curl_multi` backend remains planned. There is no benchmark data yet.

## Build

Requires CMake 3.20 or newer, a C++20 compiler, and libcurl 7.85 or newer with
development headers, plus libxml2 development headers. On Debian/Ubuntu, install
`libcurl4-openssl-dev libxml2-dev`; the macOS Command Line Tools SDK includes both.
Python 3 is required when building tests (the default); no Python packages are
needed. Use `-DBUILD_TESTING=OFF` for a library/CLI build without Python.
URL operations remain local; only explicit `HttpClient::fetch` calls make requests.

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
the sequential engine is rejected. Summaries identify the engine and, for threaded
mode, the configured worker count. No throughput or speedup claims are reported.

The summary reports attempt/response/failure counts, HTML pages processed,
discoveries, and remaining queued URLs. Exit status is 1 if any fetch failed,
otherwise 0 (including a page-limit stop). HTTP 4xx/5xx responses alone do not
cause a nonzero exit. `--max-pages 0` validates the seed without making requests.
For a reproducible local demonstration that launches and cleans up the fixture:

```bash
python3 tests/http_server.py ./build/fluxcrawl --crawl-cli
python3 tests/http_server.py ./build/fluxcrawl --threaded-cli
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
finish before process shutdown; no connection pooling is implemented yet.
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

Each iteration pops FIFO work, records a fetch attempt, and processes the fetch
result. Only 2xx responses with the exact case-insensitive media type `text/html`
(ignoring surrounding whitespace and parameters) are extracted. At maximum depth,
the response is recorded without extracting links. Other statuses, missing types,
and non-HTML types never produce discoveries. Extraction uses the effective URL
after redirects. Fetch errors are recorded and the remaining queue continues;
there are no retries. Malformed HTML uses the existing parser's recovery behavior.

All discoveries go through `CrawlState::discover_from`; no second seen set exists.
Depth 0 is the seed, and limits are inclusive. FIFO and parser document order yield
BFS. `max_pages` counts logical fetch attempts, including failures and HTTP errors;
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
These semantics are the reference behavior shared by both engines.
No robots.txt, rate limiting, or retries are implemented.

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

The private coordinator has one mutex protecting the frontier (including a single
lookahead item), seen set, active count, attempt count, fatal error, and result
aggregation. Claiming an item reserves its attempt budget and increments active
work in the same critical section. Fetching and parsing run outside the lock.
Publishing performs the canonical seen check, insertion, enqueue, and result
update under the lock, then decrements active work and notifies all waiters.

The condition-variable predicate wakes for a fatal error, an exhausted attempt
budget, no active work, or available FIFO work at the active depth. A worker exits
normally only when the budget is spent or no queued/active work remains. Temporary
frontier emptiness never implies termination while another worker can discover
links. Existing claims finish even after the budget is reached; all their results
and discoveries are published. There is no busy waiting or detached work.

Workers run concurrently within each depth level; the next level waits for the
current active level to finish. This small depth barrier preserves shortest first
discovery and the sequential reachable set under depth limits: otherwise a fast
long path can mark a URL seen too deeply before a slow shorter path arrives.
It does not impose ordering within a level. FIFO acquisition, publication, and
record order are scheduling-dependent; threaded records use publication order.
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
than a proof of all possible schedules. No async I/O or benchmark framework is
implemented.

## Layout

- `include/fluxcrawl/`: public version, URL, and HTML extractor headers.
- `src/url.cpp`: URL implementation in the compiled `fluxcrawl_core` library.
- `src/html_parser.cpp`: libxml2 HTML extraction, also in `fluxcrawl_core`.
- `include/fluxcrawl/http_client.hpp`, `http_response.hpp`, and `src/http_client.cpp`:
  blocking HTTP API and implementation.
- `include/fluxcrawl/crawl_frontier.hpp` and `crawl_state.hpp`: public crawl work
  and state APIs, implemented by the corresponding `.cpp` files in `src/`.
- `src/main.cpp`: thin command-line entrypoint.
- `include/fluxcrawl/sequential_crawler.hpp` and `src/sequential_crawler.cpp`:
  sequential orchestration.
- `include/fluxcrawl/crawl_types.hpp`: shared options and metadata-only results.
- `include/fluxcrawl/thread_pool_crawler.hpp` and `src/thread_pool_crawler.cpp`:
  fixed worker pool and private crawl coordinator.
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
