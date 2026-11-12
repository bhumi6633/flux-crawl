# Direct dependency inventory

FluxCrawl's own code is under the repository's MIT license. Dependencies are
obtained from the system package manager or the locked npm dependency tree;
the source release does not vendor their source or ship linked native binaries.
Keep each dependency's supplied notices when distributing its code or binaries.

| Dependency | Purpose | Upstream license / notice source |
| --- | --- | --- |
| libcurl | HTTP(S), URL API, curl multi event loop | curl license: https://curl.se/docs/copyright.html |
| libxml2 | Tolerant non-network HTML parsing | MIT: https://gitlab.gnome.org/GNOME/libxml2/-/blob/master/Copyright |
| SQLite | Single-process durable state | Public-domain dedication: https://sqlite.org/copyright.html |
| Boost (Beast, Asio, JSON) | Optional local telemetry server and JSON | Boost Software License 1.0: https://www.boost.org/LICENSE_1_0.txt |
| React and ReactDOM | Dashboard rendering | MIT; installed packages' LICENSE files |
| TypeScript | Dashboard type checking, build only | Apache-2.0; installed package's LICENSE.txt |
| Vite | Dashboard asset pipeline, build only | MIT plus bundled dependency notices; installed package's LICENSE.md |
| Node type definitions | Frontend test/build types only | MIT; installed package's LICENSE |

`dashboard/package-lock.json` pins transitive npm packages and their license
metadata. npm preserves package notices; React's distributed production code
also retains its license comments. A source release requires a separate frontend
build; this file is an inventory, not a claim that native/system dependencies
are relicensed by FluxCrawl.

The unbuilt `lab4`/`lab5` coursework and third-party examples were removed from
the release tree. Their history remains in Git, with the original attribution.
No FluxCrawl build target or test referenced those directories. The crawler core
uses the independently structured C++ RAII implementation under `src/`.
