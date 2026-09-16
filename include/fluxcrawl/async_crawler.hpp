#pragma once

#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/http_client.hpp>

namespace fluxcrawl {

// One caller-thread event loop, up to concurrency simultaneous curl_multi transfers.
class AsyncCrawler {
public:
    // Invalid concurrency/options or global initialization failure throw.
    explicit AsyncCrawler(std::size_t concurrency = 8, HttpClientOptions http_options = {});
    // Ordinary transfer errors become records; fatal engine failures throw.
    [[nodiscard]] CrawlResult crawl(const Url& seed, CrawlOptions options = {}) const;

private:
    std::size_t concurrency_;
    HttpClientOptions http_options_;
};

} // namespace fluxcrawl
