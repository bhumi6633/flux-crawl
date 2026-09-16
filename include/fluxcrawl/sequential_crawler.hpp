#pragma once

#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/http_client.hpp>

namespace fluxcrawl {

class SequentialCrawler {
public:
    explicit SequentialCrawler(HttpClientOptions http_options = {});
    [[nodiscard]] CrawlResult crawl(const Url& seed, const CrawlOptions& options = {}) const;

private:
    HttpClient client_;
};

} // namespace fluxcrawl
