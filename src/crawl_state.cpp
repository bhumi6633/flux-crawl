#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/host_scheduler.hpp>
#include <fluxcrawl/persistence_events.hpp>

namespace fluxcrawl {

CrawlState::CrawlState(Url seed, std::size_t max_depth) : max_depth_{max_depth}
{
    (void)discover(seed, 0);
}

std::optional<CrawlItem> CrawlState::next()
{
    return frontier_.pop();
}

void CrawlState::schedule_pending(HostScheduler& scheduler)
{
    while (auto item = next()) { scheduler.enqueue(std::move(*item)); }
}

bool CrawlState::discover(const Url& url, std::size_t depth)
{
    if (depth > max_depth_) {
        return false;
    }
    const auto [entry, inserted] = seen_urls_.insert(url);
    if (!inserted) {
        return false;
    }
    try {
        frontier_.push(CrawlItem{url, depth});
        if (sink_) { sink_->discovered({url, depth}); }
    } catch (...) {
        // Allocation failure must not leave a URL marked seen without queued work.
        seen_urls_.erase(entry);
        throw;
    }
    return true;
}

bool CrawlState::discover_from(const CrawlItem& parent, const Url& url)
{
    // Check before addition, including when max_depth_ is SIZE_MAX.
    if (parent.depth >= max_depth_) {
        return false;
    }
    return discover(url, parent.depth + 1);
}

std::vector<Url> CrawlState::seen_urls() const { return {seen_urls_.begin(), seen_urls_.end()}; }
void CrawlState::restore_seen(const std::vector<Url>& urls)
{
    frontier_ = CrawlFrontier{};
    seen_urls_ = std::unordered_set<Url>{urls.begin(), urls.end()};
}
bool CrawlState::has_seen(const Url& url) const
{
    return seen_urls_.contains(url);
}

} // namespace fluxcrawl
