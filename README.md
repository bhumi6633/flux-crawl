# FluxCrawl

A C++20 network-concurrency and performance-engineering project that implements
the same crawling workload with threaded blocking I/O and event-driven
asynchronous I/O.

**Under development.** The project provides build infrastructure, version
information, a minimal CLI, an HTTP/HTTPS URL value type, and HTML anchor link
extraction, plus in-memory FIFO crawl state and global discovery deduplication.
The planned thread-pool backend
and single-threaded libcurl `curl_multi` backend are not implemented. There is
no HTTP fetching, crawling, or benchmark data yet.

## Build

Requires CMake 3.20 or newer, a C++20 compiler, and libcurl 7.62 or newer with
development headers, plus libxml2 development headers. On Debian/Ubuntu, install
`libcurl4-openssl-dev libxml2-dev`; the macOS Command Line Tools SDK includes both.
Only libcurl's URL API is used; URL
operations do not initialize a network client or make requests.

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
```

The version is defined in `include/fluxcrawl/version.hpp`. Help and version
requests exit successfully; invalid options, missing or extra seed arguments,
and crawl requests exit with status 1. Seed arguments are validated with `Url`;
invalid or unsupported URLs produce `invalid or unsupported seed URL`. A valid
seed produces `crawl engine not implemented yet` and performs no network activity. Arguments
are checked before displaying help or version; help takes precedence if both
flags are present.

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
items, not that popped work has finished. Future engines must account for
in-flight work, synchronization, and concurrent completion order separately.
No networking, completed-request tracking, or production crawl loop is included.

## Layout

- `include/fluxcrawl/`: public version, URL, and HTML extractor headers.
- `src/url.cpp`: URL implementation in the compiled `fluxcrawl_core` library.
- `src/html_parser.cpp`: libxml2 HTML extraction, also in `fluxcrawl_core`.
- `include/fluxcrawl/crawl_frontier.hpp` and `crawl_state.hpp`: public crawl work
  and state APIs, implemented by the corresponding `.cpp` files in `src/`.
- `src/main.cpp`: thin command-line entrypoint.
- `cmake/`: target-scoped compiler warning configuration.
- `tests/`: offline CTest CLI, version, URL, and HTML extraction checks, without
  a test framework. HTML tests block external resource loading with a test-only
  guard and exercise malformed markup, entities, ordering, and deduplication.
  Crawl-state tests cover FIFO/BFS, depth boundaries, discovery-time deduplication,
  and composition with in-memory parser results.

Existing local `lab4/` and `lab5/` coursework is separate from FluxCrawl and is
not included in its build.

Released under the [MIT license](LICENSE).
