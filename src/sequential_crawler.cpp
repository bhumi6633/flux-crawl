#include <fluxcrawl/sequential_crawler.hpp>

#include <fluxcrawl/crawl_state.hpp>
#include "crawl_processing.hpp"

#include <utility>

namespace fluxcrawl {

SequentialCrawler::SequentialCrawler(HttpClientOptions http_options) : client_{std::move(http_options)} {}

CrawlResult SequentialCrawler::crawl(const Url& seed, const CrawlOptions& options) const
{
    const auto started = std::chrono::steady_clock::now();
    CrawlState state{seed, options.max_depth};
    CrawlResult result;
    while (!state.empty() && result.pages_attempted < options.max_pages) {
        auto item = state.next();
        if (!item) {
            break;
        }
        ++result.pages_attempted;
        detail::publish_page(state, result, detail::process_page(client_, std::move(*item), options), seed, options);
    }
    result.urls_discovered = state.seen_count();
    result.urls_pending = state.frontier_size();
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

} // namespace fluxcrawl
