#pragma once

#include <fluxcrawl/crawl_frontier.hpp>

#include <fluxcrawl/backpressure.hpp>
#include <fluxcrawl/discovery_spool.hpp>
#include <unordered_set>
#include <vector>

namespace fluxcrawl {
class HostScheduler;
class CrawlMutationSink;

// Single-threaded discovery state. Seen means accepted, not fetched or completed.
class CrawlState {
public:
    CrawlState(Url seed, std::size_t max_depth, BackpressureOptions pressure = {.enabled = false});
    void configure_spool(const std::filesystem::path& prefix, bool persistent);
    void defer(const Url& url, std::size_t depth);
    bool admit_deferred(std::size_t unfinished);
    void note_discovery_io(std::chrono::nanoseconds time) { metrics_.deferred_io_time += time; }
    void observe(std::size_t unfinished, std::size_t pending);
    bool has_deferred() const { return spool_ && spool_->size() != 0; }
    std::shared_ptr<DiscoverySpool> spool() const { return spool_; }
    QueuePressureMetrics pressure_metrics() const;

    void set_mutation_sink(CrawlMutationSink* sink) noexcept { sink_ = sink; }
    [[nodiscard]] std::optional<CrawlItem> next();
    // Move accepted discoveries into scheduling; seen identity remains here.
    void schedule_pending(HostScheduler& scheduler);
    // False means duplicate or beyond the inclusive depth limit; state is unchanged.
    [[nodiscard]] bool discover(const Url& url, std::size_t depth);
    [[nodiscard]] bool discover_from(const CrawlItem& parent, const Url& url);
    [[nodiscard]] std::vector<Url> seen_urls() const;
    void restore_seen(const std::vector<Url>& urls);
    [[nodiscard]] bool has_seen(const Url& url) const;
    [[nodiscard]] std::size_t frontier_size() const noexcept { return frontier_.size(); }
    [[nodiscard]] std::size_t seen_count() const noexcept { return seen_urls_.size(); }
    // Describes queued work only; no in-flight work is tracked.
    [[nodiscard]] bool empty() const noexcept { return frontier_.empty(); }

private:
    BackpressureOptions pressure_options_;
    PressureGate gate_;
    QueuePressureMetrics metrics_;
    std::shared_ptr<DiscoverySpool> spool_;
    CrawlMutationSink* sink_ = nullptr;
    CrawlFrontier frontier_;
    std::unordered_set<Url> seen_urls_;
    std::size_t max_depth_;
};

} // namespace fluxcrawl
