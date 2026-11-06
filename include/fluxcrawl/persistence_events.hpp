#pragma once
#include <fluxcrawl/crawl_frontier.hpp>
#include <chrono>
#include <optional>
namespace fluxcrawl {
// Logical transition observer. No filesystem or database API belongs in hot state.
// Serialized by the same owner/lock as CrawlState and HostScheduler.
class CrawlMutationSink {
public:
    virtual ~CrawlMutationSink() = default;
    virtual void discovered(const CrawlItem& item) = 0;
    virtual void work(const CrawlItem& item, std::optional<std::chrono::steady_clock::time_point> deadline, bool active) = 0;
    virtual void completed(const CrawlItem& item) = 0;
    virtual void activity() = 0;
};
} // namespace fluxcrawl
