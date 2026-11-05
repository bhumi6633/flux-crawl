#pragma once
#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/crawl_policy.hpp>
#include <mutex>

namespace fluxcrawl::detail {
inline bool checkpointing(const CrawlOptions& options)
{ return !options.checkpoint.path.empty() || bool(options.checkpoint.on_snapshot); }
void reserve_attempt(CrawlResult& result, const CrawlItem& item);
void restore_session(const Url& seed, const CrawlOptions& options, const HttpClientOptions& http,
                     CrawlState& state, HostScheduler& scheduler, CrawlPolicy& policy, CrawlResult& result);
class CheckpointRuntime {
public:
    struct Report { std::size_t bytes = 0; std::string error; };
    CheckpointRuntime(const Url& seed, const CrawlOptions& options, const HttpClientOptions& http)
        : seed_{seed}, options_{options}, http_{http}, sequence_{options.resume_from ? options.resume_from->sequence : 0},
          last_attempts_{options.resume_from ? options.resume_from->counters.pages_attempted : 0} {}
    bool stop(const CrawlResult& result) const {
        return options_.checkpoint.stop_after_attempts && result.pages_attempted >= *options_.checkpoint.stop_after_attempts;
    }
    // Caller holds the crawl lock. No file I/O, serialization or callbacks here.
    std::optional<CrawlCheckpoint> capture(const CrawlState& state, const HostScheduler& scheduler,
                                         const CrawlPolicy& policy, const CrawlResult& result, bool final = false);
    // Separate writer lock orders concurrent snapshots without holding crawl lock.
    Report write(const CrawlCheckpoint& snapshot);
    static void report(CrawlResult& result, Report report);
private:
    const Url& seed_;
    const CrawlOptions& options_;
    const HttpClientOptions& http_;
    std::uint64_t sequence_, last_written_ = 0;
    std::size_t last_attempts_;
    std::mutex writer_;
};
} // namespace fluxcrawl::detail
