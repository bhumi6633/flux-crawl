# FluxCrawl

A C++20 network-concurrency and performance-engineering project that implements
the same crawling workload with threaded blocking I/O and event-driven
asynchronous I/O.

**Under development.** The project provides build infrastructure, version
information, a minimal CLI, an HTTP/HTTPS URL value type, and HTML anchor link
extraction. The planned thread-pool backend
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

## Layout

- `include/fluxcrawl/`: public version, URL, and HTML extractor headers.
- `src/url.cpp`: URL implementation in the compiled `fluxcrawl_core` library.
- `src/html_parser.cpp`: libxml2 HTML extraction, also in `fluxcrawl_core`.
- `src/main.cpp`: thin command-line entrypoint.
- `cmake/`: target-scoped compiler warning configuration.
- `tests/`: CTest CLI, version, and URL checks, without a test framework.

Existing local `lab4/` and `lab5/` coursework is separate from FluxCrawl and is
not included in its build.

Released under the [MIT license](LICENSE).
