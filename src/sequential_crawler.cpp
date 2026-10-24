#include <fluxcrawl/sequential_crawler.hpp>

#include <fluxcrawl/crawl_state.hpp>
#include "crawl_processing.hpp"
#include "profile_timer.hpp"

#include <utility>
#include <thread>
#include <stdexcept>

namespace fluxcrawl {

SequentialCrawler::SequentialCrawler(HttpClientOptions http_options) : client_{std::move(http_options)} {}

CrawlResult SequentialCrawler::crawl(const Url& seed, const CrawlOptions& options) const
{
    const auto started = std::chrono::steady_clock::now();
    CrawlState state{seed, options.max_depth};
    HostScheduler scheduler{1, options.scheduling};
    state.schedule_pending(scheduler);
    CrawlResult result;
    if (options.profile) { result.profile.emplace(); }
    auto* profile = result.profile ? &result.profile->common : nullptr;
    while (scheduler.pending() != 0 && result.pages_attempted < options.max_pages) {
        auto item = [&] {
            detail::ProfileTimer timer{profile ? &profile->frontier : nullptr};
            return scheduler.acquire(HostScheduler::Clock::now());
        }();
        if (!item) {
            if (const auto deadline = scheduler.next_deadline(HostScheduler::Clock::now())) {
                std::this_thread::sleep_until(*deadline);
                continue;
            }
            throw std::logic_error("Pending sequential work has no scheduling deadline");
        }
        ++result.pages_attempted;
        detail::publish_page(state, result, detail::process_page(client_, std::move(item->item), options, profile), seed, options, profile);
        state.schedule_pending(scheduler);
    }
    result.urls_discovered = state.seen_count();
    result.urls_pending = scheduler.pending();
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

} // namespace fluxcrawl
