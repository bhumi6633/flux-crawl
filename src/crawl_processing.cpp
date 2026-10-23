#include "crawl_processing.hpp"
#include "profile_timer.hpp"

#include <fluxcrawl/html_parser.hpp>
#include <utility>

namespace fluxcrawl::detail {

ProcessedPage process_page(const HttpClient& client, CrawlItem item, const CrawlOptions& options, CommonProfile* profile)
{
    const auto started = std::chrono::steady_clock::now();
    auto fetched = client.fetch(item.url);
    const auto latency = std::chrono::steady_clock::now() - started;
    return process_response(std::move(fetched), std::move(item), options, latency, profile);
}

ProcessedPage process_response(HttpResult fetched, CrawlItem item, const CrawlOptions& options,
                               std::chrono::nanoseconds latency, CommonProfile* profile)
{
    if (auto* error = std::get_if<HttpError>(&fetched)) {
        return {{std::move(item), std::move(*error), latency}, {}, false};
    }
    auto& response = std::get<HttpResponse>(fetched);
    const bool parse = response.status_code >= 200 && response.status_code < 300
        && is_html_content_type(response.content_type) && item.depth < options.max_depth;
    std::vector<Url> links;
    if (parse) {
        ProfileTimer timer{profile ? &profile->extraction : nullptr};
        links = HtmlLinkExtractor{}.extract_links(response.body, response.effective_url);
        if (profile) { ++profile->extractions; }
    }
    return {{std::move(item), CrawlResponseMetadata{std::move(response.effective_url),
             response.status_code, std::move(response.content_type), response.downloaded_bytes}, latency},
            std::move(links), parse};
}

void publish_page(CrawlState& state, CrawlResult& result, ProcessedPage page,
                  const Url& seed, const CrawlOptions& options, CommonProfile* profile)
{
    if (std::holds_alternative<HttpError>(page.record.outcome)) {
        ++result.fetch_failures;
    } else {
        ++result.responses_received;
        result.bytes_downloaded += std::get<CrawlResponseMetadata>(page.record.outcome).downloaded_bytes;
    }
    if (page.html_processed) { ++result.html_pages_processed; }
    {
        ProfileTimer timer{profile ? &profile->discovery : nullptr};
        for (const auto& link : page.links) {
            if (profile) { ++profile->links_considered; }
            if (!options.same_host_only || link.host() == seed.host()) {
                const bool accepted = state.discover_from(page.record.item, link);
                if (profile && accepted) { ++profile->links_accepted; }
            }
        }
    }
    result.records.push_back(std::move(page.record));
}

} // namespace fluxcrawl::detail
