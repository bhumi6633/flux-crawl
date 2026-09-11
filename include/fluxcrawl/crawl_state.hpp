#pragma once

#include <fluxcrawl/crawl_frontier.hpp>

#include <unordered_set>

namespace fluxcrawl {

// Single-threaded discovery state. Seen means accepted, not fetched or completed.
class CrawlState {
public:
    CrawlState(Url seed, std::size_t max_depth);

    [[nodiscard]] std::optional<CrawlItem> next();
    // False means duplicate or beyond the inclusive depth limit; state is unchanged.
    [[nodiscard]] bool discover(const Url& url, std::size_t depth);
    [[nodiscard]] bool discover_from(const CrawlItem& parent, const Url& url);
    [[nodiscard]] bool has_seen(const Url& url) const;
    [[nodiscard]] std::size_t frontier_size() const noexcept { return frontier_.size(); }
    [[nodiscard]] std::size_t seen_count() const noexcept { return seen_urls_.size(); }
    // Describes queued work only; no in-flight work is tracked.
    [[nodiscard]] bool empty() const noexcept { return frontier_.empty(); }

private:
    CrawlFrontier frontier_;
    std::unordered_set<Url> seen_urls_;
    std::size_t max_depth_;
};

} // namespace fluxcrawl
