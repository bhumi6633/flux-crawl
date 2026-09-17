#include "crawl_processing.hpp"

#include <fluxcrawl/html_parser.hpp>
#include <utility>

namespace fluxcrawl::detail {

ProcessedPage process_page(const HttpClient& client, CrawlItem item, const CrawlOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    auto fetched = client.fetch(item.url);
    const auto latency = std::chrono::steady_clock::now() - started;
    return process_response(std::move(fetched), std::move(item), options, latency);
}

ProcessedPage process_response(HttpResult fetched, CrawlItem item, const CrawlOptions& options,
                               std::chrono::nanoseconds latency)
{
    if (auto* error = std::get_if<HttpError>(&fetched)) {
        return {{std::move(item), std::move(*error), latency}, {}, false};
    }
    auto& response = std::get<HttpResponse>(fetched);
    const bool parse = response.status_code >= 200 && response.status_code < 300
        && is_html_content_type(response.content_type) && item.depth < options.max_depth;
    auto links = parse ? HtmlLinkExtractor{}.extract_links(response.body, response.effective_url)
                       : std::vector<Url>{};
    return {{std::move(item), CrawlResponseMetadata{std::move(response.effective_url),
             response.status_code, std::move(response.content_type), response.downloaded_bytes}, latency},
            std::move(links), parse};
}

void publish_page(CrawlState& state, CrawlResult& result, ProcessedPage page,
                  const Url& seed, const CrawlOptions& options)
{
    if (std::holds_alternative<HttpError>(page.record.outcome)) {
        ++result.fetch_failures;
    } else {
        ++result.responses_received;
        result.bytes_downloaded += std::get<CrawlResponseMetadata>(page.record.outcome).downloaded_bytes;
    }
    if (page.html_processed) { ++result.html_pages_processed; }
    for (const auto& link : page.links) {
        if (!options.same_host_only || link.host() == seed.host()) {
            (void)state.discover_from(page.record.item, link);
        }
    }
    result.records.push_back(std::move(page.record));
}

} // namespace fluxcrawl::detail
