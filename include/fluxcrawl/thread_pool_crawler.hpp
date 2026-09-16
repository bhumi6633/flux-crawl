#pragma once

#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/http_client.hpp>

namespace fluxcrawl {

class ThreadPoolCrawler {
public:
    explicit ThreadPoolCrawler(std::size_t workers = 4, HttpClientOptions http_options = {});
    [[nodiscard]] CrawlResult crawl(const Url& seed, const CrawlOptions& options = {}) const;

private:
    std::size_t workers_;
    HttpClient client_;
};

} // namespace fluxcrawl
