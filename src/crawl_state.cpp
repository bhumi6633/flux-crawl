#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/host_scheduler.hpp>
#include <fluxcrawl/persistence_events.hpp>

namespace fluxcrawl {

CrawlState::CrawlState(Url seed, std::size_t max_depth, BackpressureOptions pressure) : pressure_options_{pressure}, gate_{pressure}, max_depth_{max_depth}
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

void CrawlState::configure_spool(const std::filesystem::path& prefix, bool persistent)
{
    if (pressure_options_.enabled) { spool_ = std::make_shared<DiscoverySpool>(prefix, persistent); }
}
void CrawlState::defer(const Url& url, std::size_t depth)
{
    if (depth > max_depth_ || has_seen(url)) { return; }
    if (!spool_) { (void)discover(url, depth); return; }
    spool_->append({url, depth});
    metrics_.deferred_urls = spool_->size(); metrics_.peak_deferred = std::max(metrics_.peak_deferred, metrics_.deferred_urls);
}
bool CrawlState::admit_deferred(std::size_t unfinished)
{
    gate_.update(unfinished);
    if (!spool_ || !gate_.admits(unfinished)) { return false; }
    for (std::size_t scanned = 0; scanned < 64; ++scanned) {
        auto item = spool_->pop(gate_.has_activated()); if (!item) { break; }
        if (discover(item->url, item->depth)) { ++metrics_.deferred_admissions; return true; }
    }
    return false;
}
void CrawlState::observe(std::size_t unfinished, std::size_t pending)
{
    gate_.update(unfinished);
    metrics_.current_pending = pending; metrics_.peak_pending = std::max(metrics_.peak_pending, pending);
    metrics_.peak_unfinished = std::max(metrics_.peak_unfinished, unfinished);
    metrics_.peak_seen = std::max(metrics_.peak_seen, seen_count());
}
QueuePressureMetrics CrawlState::pressure_metrics() const
{
    auto copy = metrics_; gate_.metrics(copy); copy.deferred_urls = spool_ ? spool_->size() : 0; if (spool_) { copy.deferred_io_time += spool_->io_time(); } return copy;
}

} // namespace fluxcrawl
