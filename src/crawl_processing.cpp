#include "crawl_processing.hpp"
#include "profile_timer.hpp"

#include <fluxcrawl/html_parser.hpp>
#include <utility>
#include <random>
#include <algorithm>

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
             response.status_code, std::move(response.content_type), response.downloaded_bytes, response.retry_after}, latency},
            std::move(links), parse};
}

void publish_page(CrawlState& state, CrawlResult& result, ProcessedPage page,
                  const Url& seed, const CrawlOptions& options, HostScheduler& scheduler, CommonProfile* profile)
{
    if (page.record.item.retry_count != 0) { ++result.retries_attempted; }
    const auto* response = std::get_if<CrawlResponseMetadata>(&page.record.outcome);
    const auto* error = std::get_if<HttpError>(&page.record.outcome);
    const bool transient = response ? retryable_status(response->status_code) : retryable_error(*error);
    if (profile) { ++profile->retry_decisions; }
    const auto count = page.record.item.retry_count;
    if (transient && count < options.retry.max_retries) {
        // Per-thread RNG initialized once; no shared RNG contention or data races.
        const auto sample = [&] {
            if (!options.retry.jitter) { return 1.0; }
            thread_local std::mt19937_64 random{std::random_device{}()};
            return std::generate_canonical<double, 53>(random);
        }();
        const auto backoff = options.retry.delay(count + 1, sample);
        const auto now = HostScheduler::Clock::now();
        const auto room = HostScheduler::Time::max() - now;
        auto deadline = std::chrono::duration<long double>(backoff) >= std::chrono::duration<long double>(room)
            ? HostScheduler::Time::max() : now + std::chrono::duration_cast<HostScheduler::Clock::duration>(backoff);
        if (response && response->retry_after) {
            const auto server = std::chrono::duration<long double>(*response->retry_after) >= std::chrono::duration<long double>(room)
                ? HostScheduler::Time::max() : now + std::chrono::duration_cast<HostScheduler::Clock::duration>(*response->retry_after);
            deadline = std::max(deadline, server);
            ++result.retry_after_used;
        }
        auto retry = page.record.item;
        ++retry.retry_count;
        scheduler.delay(std::move(retry), deadline);
        ++result.retries_scheduled;
        if (profile) {
            const auto delay = std::chrono::duration_cast<ProfileDuration>(deadline - now);
            profile->retry_delay += std::min(delay, ProfileDuration::max() - profile->retry_delay);
        }
    } else {
        ++result.resources_completed;
        if (transient && options.retry.max_retries != 0) { ++result.retries_exhausted; }
        if (error) { ++result.terminal_fetch_failures; }
        if (response && response->status_code >= 400) { ++result.terminal_http_failures; }
        if (count != 0 && response && response->status_code >= 200 && response->status_code < 300) {
            ++result.responses_recovered_after_retry;
        }
    }
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
