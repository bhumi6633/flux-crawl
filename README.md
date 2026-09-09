# FluxCrawl

A C++20 network-concurrency and performance-engineering project that implements
the same crawling workload with threaded blocking I/O and event-driven
asynchronous I/O.

**Under development.** The current bootstrap provides build infrastructure,
version information, and a minimal CLI only. The planned thread-pool backend
and single-threaded libcurl `curl_multi` backend are not implemented. There is
no URL parsing, HTTP fetching, HTML parsing, crawling, or benchmark data yet.

## Build

Requires CMake 3.20 or newer and a C++20 compiler. No third-party libraries are
required.

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
and crawl requests exit with status 1. A seed argument produces the message
`crawl engine not implemented yet` and performs no network activity. Arguments
are checked before displaying help or version; help takes precedence if both
flags are present.

## Layout

- `include/fluxcrawl/`: public core headers. `fluxcrawl_core` is currently a
  header-only CMake target, ready to become a compiled library when core logic
  is introduced.
- `src/main.cpp`: thin command-line entrypoint.
- `cmake/`: target-scoped compiler warning configuration.

Existing local `lab4/` and `lab5/` coursework is separate from FluxCrawl and is
not included in its build.

Released under the [MIT license](LICENSE).
