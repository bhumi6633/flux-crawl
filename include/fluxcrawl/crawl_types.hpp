#pragma once

#include <fluxcrawl/crawl_frontier.hpp>
#include <fluxcrawl/http_response.hpp>

#include <fluxcrawl/profiling.hpp>
#include <vector>
#include <chrono>

namespace fluxcrawl {

struct CrawlOptions {
    std::size_t max_depth = 2;
    std::size_t max_pages = 100;
    bool same_host_only = false;
    bool profile = false;
};

struct CrawlResponseMetadata {
    Url effective_url;
    long status_code;
    std::string content_type;
    std::size_t downloaded_bytes;
};

struct CrawlRecord {
    CrawlItem item;
    std::variant<CrawlResponseMetadata, HttpError> outcome;
    // Active transfer duration; excludes frontier wait and HTML processing.
    std::chrono::nanoseconds latency{};
};

struct CrawlResult {
    std::size_t pages_attempted = 0;
    std::size_t responses_received = 0;
    // All HttpError results, including transport, body-limit, and metadata failures.
    std::size_t fetch_failures = 0;
    std::size_t html_pages_processed = 0;
    std::size_t urls_discovered = 0; // Accepted by CrawlState, including the seed.
    std::size_t urls_pending = 0;
    std::chrono::nanoseconds elapsed{};
    // Sum of decoded body bytes in HttpResponse outcomes; excludes partial failed bodies.
    std::size_t bytes_downloaded = 0;
    std::optional<ProfileSummary> profile{};
    std::vector<CrawlRecord> records; // Sequential attempt order or concurrent publication order; no bodies.
};

} // namespace fluxcrawl
