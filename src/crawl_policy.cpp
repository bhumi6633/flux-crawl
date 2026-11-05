#include <fluxcrawl/crawl_policy.hpp>
#include <fluxcrawl/crawler_identity.hpp>

namespace fluxcrawl {
void CrawlPolicy::schedule(CrawlState& discovery, HostScheduler& scheduler, CrawlResult& result)
{
    while (auto item = discovery.next()) { accept(std::move(*item), scheduler, result); }
}
void CrawlPolicy::accept(CrawlItem item, HostScheduler& scheduler, CrawlResult& result)
{
    if (!enabled_) { scheduler.enqueue(std::move(item)); return; }
    auto [found, inserted] = entries_.try_emplace(HostKey::from(item.url));
    auto& entry = found->second;
    if (!inserted) { ++result.robots_cache_hits; }
    if (entry.state != State::Fetching) { admit(std::move(item), entry, scheduler, result); return; }
    if (inserted) {
        const auto robots = Url::resolve(item.url, "/robots.txt").value();
        scheduler.enqueue({robots, item.depth, 0, true});
        ++result.robots_origins;
    }
    entry.waiting.push_back(std::move(item));
}
void CrawlPolicy::admit(CrawlItem item, const Entry& entry, HostScheduler& scheduler, CrawlResult& result)
{
    if (entry.deny_all || !entry.rules.allowed(CrawlerIdentity::product_token, item.url)) {
        ++result.urls_blocked_by_robots;
    } else {
        ++result.robots_allowed;
        if (entry.reusable && item.url.path() == "/robots.txt" && !item.url.has_query()) {
            // Infrastructure response already obtained; do not manufacture a
            // resource attempt or retain a second copy of its body.
            ++result.robots_resources_reused;
        } else { scheduler.enqueue(std::move(item)); }
    }
}
void CrawlPolicy::resolve(const CrawlRecord& bootstrap, std::optional<RobotsPolicy> rules,
                          HostScheduler& scheduler, CrawlResult& result)
{
    auto& entry = entries_.at(HostKey::from(bootstrap.item.url));
    const auto* response = std::get_if<CrawlResponseMetadata>(&bootstrap.outcome);
    if (response && ((response->status_code >= 200 && response->status_code < 300)
        || response->status_code == 404 || response->status_code == 410)) {
        entry.state = State::Ready;
        if (rules) { entry.rules = std::move(*rules); }
        entry.reusable = true;
    } else {
        entry.state = State::Unavailable;
        entry.deny_all = true;
        ++result.robots_fetch_failures;
    }
    for (auto& item : entry.waiting) { admit(std::move(item), entry, scheduler, result); }
    entry.waiting.clear();
}
std::size_t CrawlPolicy::held() const noexcept
{
    std::size_t count = 0;
    for (const auto& [key, entry] : entries_) { (void)key; count += entry.waiting.size(); }
    return count;
}
} // namespace fluxcrawl
