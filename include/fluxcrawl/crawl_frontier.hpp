#pragma once

#include <fluxcrawl/url.hpp>

#include <cstddef>
#include <deque>
#include <optional>

namespace fluxcrawl {

struct CrawlItem {
    Url url;
    std::size_t depth;
    std::size_t retry_count = 0; // Attempt state, excluded from canonical identity.
};

// Single-threaded FIFO work storage; discovery policy belongs to CrawlState.
class CrawlFrontier {
public:
    void push(CrawlItem item);
    [[nodiscard]] std::optional<CrawlItem> pop();
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

private:
    std::deque<CrawlItem> items_;
};

} // namespace fluxcrawl
