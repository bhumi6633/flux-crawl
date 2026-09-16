#pragma once

#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/http_client.hpp>

namespace fluxcrawl::detail {

struct ProcessedPage {
    CrawlRecord record;
    std::vector<Url> links;
    bool html_processed = false;
};

ProcessedPage process_page(const HttpClient& client, CrawlItem item, const CrawlOptions& options);
void publish_page(CrawlState& state, CrawlResult& result, ProcessedPage page,
                  const Url& seed, const CrawlOptions& options);

} // namespace fluxcrawl::detail
