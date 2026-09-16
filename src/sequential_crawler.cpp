#include <fluxcrawl/sequential_crawler.hpp>

#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/html_parser.hpp>

#include <utility>

namespace fluxcrawl {

SequentialCrawler::SequentialCrawler(HttpClientOptions http_options) : client_{std::move(http_options)} {}

CrawlResult SequentialCrawler::crawl(const Url& seed, const CrawlOptions& options) const
{
    CrawlState state{seed, options.max_depth};
    const HtmlLinkExtractor extractor;
    CrawlResult result;
    while (!state.empty() && result.pages_attempted < options.max_pages) {
        auto item = state.next();
        if (!item) {
            break;
        }
        ++result.pages_attempted;
        auto fetched = client_.fetch(item->url);
        if (auto* error = std::get_if<HttpError>(&fetched)) {
            ++result.fetch_failures;
            result.records.push_back({std::move(*item), std::move(*error)});
            continue;
        }
        auto& response = std::get<HttpResponse>(fetched);
        ++result.responses_received;
        if (response.status_code >= 200 && response.status_code < 300
            && is_html_content_type(response.content_type) && item->depth < options.max_depth) {
            ++result.html_pages_processed;
            const auto links = extractor.extract_links(response.body, response.effective_url);
            for (const auto& link : links) {
                if (!options.same_host_only || link.host() == seed.host()) {
                    (void)state.discover_from(*item, link);
                }
            }
        }
        result.records.push_back({std::move(*item), CrawlResponseMetadata{
            std::move(response.effective_url), response.status_code,
            std::move(response.content_type), response.downloaded_bytes}});
    }
    result.urls_discovered = state.seen_count();
    result.urls_pending = state.frontier_size();
    return result;
}

} // namespace fluxcrawl
