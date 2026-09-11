#include <fluxcrawl/crawl_frontier.hpp>

#include <utility>

namespace fluxcrawl {

void CrawlFrontier::push(CrawlItem item)
{
    items_.push_back(std::move(item));
}

std::optional<CrawlItem> CrawlFrontier::pop()
{
    if (items_.empty()) {
        return std::nullopt;
    }
    CrawlItem item = std::move(items_.front());
    items_.pop_front();
    return item;
}

} // namespace fluxcrawl
