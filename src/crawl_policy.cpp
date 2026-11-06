#include <fluxcrawl/crawl_policy.hpp>
#include <fluxcrawl/crawler_identity.hpp>
#include <algorithm>

namespace fluxcrawl {
void CrawlPolicy::schedule(CrawlState& discovery, HostScheduler& scheduler, CrawlResult& result)
{
    while (auto item = discovery.next()) { accept({std::move(*item)}, scheduler, result); }
}
void CrawlPolicy::accept(HostScheduler::Work work, HostScheduler& scheduler, CrawlResult& result)
{
    if (!enabled_) {
        if (work.eligible_at) { scheduler.delay(std::move(work.item), *work.eligible_at); }
        else { scheduler.enqueue(std::move(work.item)); }
        return;
    }
    auto& item = work.item;
    auto [found, inserted] = entries_.try_emplace(HostKey::from(item.url));
    auto& entry = found->second;
    if (!inserted) { ++result.robots_cache_hits; }
    if (entry.state != State::Fetching) { admit(std::move(work), entry, scheduler, result); return; }
    if (inserted) {
        const auto robots = Url::resolve(item.url, "/robots.txt").value();
        scheduler.enqueue({robots, item.depth, 0, true});
        ++result.robots_origins;
    }
    entry.waiting.push_back(std::move(work));
}
void CrawlPolicy::admit(HostScheduler::Work work, const Entry& entry, HostScheduler& scheduler, CrawlResult& result)
{
    auto& item = work.item;
    const bool bootstrap_url = entry.reusable && item.url.path() == "/robots.txt" && !item.url.has_query();
    if (entry.deny_all || (!bootstrap_url && !entry.rules.allowed(CrawlerIdentity::product_token, item.url))) {
        scheduler.mark_terminal(item);
        ++result.urls_blocked_by_robots;
    } else {
        ++result.robots_allowed;
        if (bootstrap_url) {
            // Infrastructure response already obtained; do not manufacture a
            // resource attempt or retain a second copy of its body.
            scheduler.mark_terminal(item);
            ++result.robots_resources_reused;
        } else if (work.eligible_at) { scheduler.delay(std::move(item), *work.eligible_at); }
        else { scheduler.enqueue(std::move(item)); }
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
std::vector<HostScheduler::Work> CrawlPolicy::snapshot() const
{
    std::vector<HostScheduler::Work> work;
    for (const auto& [key, entry] : entries_) {
        (void)key; work.insert(work.end(), entry.waiting.begin(), entry.waiting.end());
    }
    std::sort(work.begin(), work.end(), [](const auto& a, const auto& b) {
        if (a.item.depth != b.item.depth) { return a.item.depth < b.item.depth; }
        return a.item.url.str() < b.item.url.str();
    });
    return work;
}
std::size_t CrawlPolicy::held() const noexcept
{
    std::size_t count = 0;
    for (const auto& [key, entry] : entries_) { (void)key; count += entry.waiting.size(); }
    return count;
}
} // namespace fluxcrawl
