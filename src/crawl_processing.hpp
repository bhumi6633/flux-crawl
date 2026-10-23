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

ProcessedPage process_response(HttpResult fetched, CrawlItem item, const CrawlOptions& options, std::chrono::nanoseconds latency, CommonProfile* profile = nullptr);
ProcessedPage process_page(const HttpClient& client, CrawlItem item, const CrawlOptions& options, CommonProfile* profile = nullptr);
void publish_page(CrawlState& state, CrawlResult& result, ProcessedPage page,
                  const Url& seed, const CrawlOptions& options, CommonProfile* profile = nullptr);

} // namespace fluxcrawl::detail
