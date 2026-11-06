#pragma once
#include "checkpoint_runtime.hpp"
#include "persistence_journal.hpp"
#include <atomic>
#include <deque>

namespace fluxcrawl::detail {
inline bool persistence_enabled(const CrawlOptions& options) { return checkpointing(options) || !options.sqlite.path.empty(); }
class PersistenceRuntime {
public:
    struct Request { std::optional<CrawlCheckpoint> snapshot; bool database = false; bool final = false; };
    struct Report { CheckpointRuntime::Report snapshot; PersistenceMetrics database; std::string error; bool is_database = false; };
    PersistenceRuntime(const Url& seed, const CrawlOptions& options, const HttpClientOptions& http);
    void initialize(CrawlState& state, HostScheduler& scheduler, CrawlPolicy& policy, CrawlResult& result);
    bool stop(const CrawlResult& result) const { return failed_.load() || snapshots_.stop(result); }
    std::optional<Request> capture(const CrawlState& state, const HostScheduler& scheduler, const CrawlPolicy& policy, const CrawlResult& result, bool final = false);
    Report write(const Request& request);
    static void report(CrawlResult& result, Report report);
private:
    const Url& seed_;
    const CrawlOptions& options_;
    const HttpClientOptions& http_;
    CheckpointRuntime snapshots_;
    PersistenceMode mode_;
    PersistenceJournal journal_;
    std::unique_ptr<CrawlStore> store_;
    std::uint64_t sequence_;
    std::mutex queue_mutex_, writer_mutex_;
    std::deque<std::shared_ptr<const CrawlMutationBatch>> queue_;
    std::atomic<bool> failed_{false};
};
} // namespace fluxcrawl::detail
