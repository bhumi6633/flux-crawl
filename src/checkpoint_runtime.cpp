#include "checkpoint_runtime.hpp"
#include <iostream>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fluxcrawl::detail {
namespace {
std::int64_t wall_now()
{ return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}
void reserve_attempt(CrawlResult& result, const CrawlItem& item)
{
    if (item.policy_request) {
        ++result.robots_fetches;
        if (item.retry_count) { ++result.robots_retries_attempted; }
    } else {
        ++result.pages_attempted;
        if (item.retry_count) { ++result.retries_attempted; }
        if (item.recovery) {
            ++result.recovery_attempts;
            if (item.retry_count) { ++result.recovery_retry_attempts; }
        }
    }
}
void restore_session(const Url& seed, const CrawlOptions& options, const HttpClientOptions& http,
                     CrawlState& state, HostScheduler& scheduler, CrawlPolicy& policy, CrawlResult& result)
{
    if (!options.resume_from) { policy.schedule(state, scheduler, result); return; }
    const auto& c = *options.resume_from;
    CheckpointManager::validate(c, c.external_seen_count ? std::numeric_limits<std::size_t>::max() : 1000000);
    if (seed != c.seed || !CheckpointManager::same_semantics(options, c.options)
        || http.connect_timeout != c.http.connect_timeout || http.request_timeout != c.http.request_timeout
        || http.max_redirects != c.http.max_redirects || http.max_body_bytes != c.http.max_body_bytes
        || http.user_agent != c.http.user_agent || http.use_environment_proxy != c.http.use_environment_proxy) {
        throw std::invalid_argument("Resume semantic/HTTP configuration must match checkpoint");
    }
    state.restore_seen(c.seen);
    if (state.spool()) { state.spool()->restore(c.deferred); }
    result = CheckpointManager::copy_counters(c.counters);
    result.session_start_attempts = result.pages_attempted;
    result.session_start_responses = result.responses_received;
    result.session_start_bytes = result.bytes_downloaded;
    const auto now = HostScheduler::Clock::now(); const auto wall = wall_now();
    auto unfinished = c.unfinished;
    // A fresh origin bootstrap must use that origin's shallowest held depth.
    // Active recovery entries can follow deeper queued entries in the snapshot.
    std::stable_sort(unfinished.begin(), unfinished.end(), [](const auto& a, const auto& b) {
        return a.item.depth < b.item.depth;
    });
    for (const auto& work : unfinished) {
        auto item = work.item;
        item.recovery = item.recovery || work.was_active;
        const auto deadline = work.eligible_unix_ms ? std::optional{CheckpointManager::steady_deadline(*work.eligible_unix_ms, now, wall)} : std::nullopt;
        policy.accept({std::move(item), deadline}, scheduler, result);
    }
}
std::optional<CrawlCheckpoint> CheckpointRuntime::capture(const CrawlState& state, const HostScheduler& scheduler,
                                                         const CrawlPolicy& policy, const CrawlResult& result, bool final)
{
    if (!checkpointing(options_)) { return std::nullopt; }
    if (options_.checkpoint.every == 0) { throw std::invalid_argument("Checkpoint interval must be positive"); }
    if (!final && result.pages_attempted - last_attempts_ < options_.checkpoint.every) { return std::nullopt; }
    last_attempts_ = result.pages_attempted;
    CrawlCheckpoint c{.seed = seed_};
    if (sequence_ >= std::numeric_limits<std::uint64_t>::max() - 1) { throw std::overflow_error("Checkpoint sequence exhausted"); }
    c.sequence = ++sequence_; c.options = options_; c.options.checkpoint = {}; c.options.resume_from.reset(); c.options.profile = false;
    c.deferred = state.spool() ? state.spool()->capture() : DeferredState{}; c.spool_sync = state.spool();
    c.http = http_; c.seen = state.seen_urls(); c.counters = CheckpointManager::copy_counters(result);
    auto work = scheduler.snapshot(); auto held = policy.snapshot();
    work.insert(work.end(), held.begin(), held.end());
    const auto now = HostScheduler::Clock::now(); const auto wall = wall_now();
    for (const auto& entry : work) {
        if (entry.item.policy_request) {
            if (entry.was_active) { ++c.counters.robots_attempts_unresolved; }
            continue; // Policy cache/bootstrap always restarts Unknown.
        }
        c.unfinished.push_back({entry.item, entry.eligible_at ? std::optional{CheckpointManager::wall_deadline(*entry.eligible_at, now, wall)} : std::nullopt, entry.was_active});
        if (entry.was_active) { ++c.counters.attempts_unresolved; }
    }
    c.counters.urls_discovered = c.seen.size(); c.counters.urls_pending = c.unfinished.size();
    c.counters.retries_cancelled = c.counters.retries_scheduled + c.counters.recovery_retry_attempts - c.counters.retries_attempted;
    c.counters.robots_retries_cancelled = c.counters.robots_retries_scheduled - c.counters.robots_retries_attempted;
    c.completed = c.unfinished.empty() && c.deferred.count() == 0;
    return c;
}
CheckpointRuntime::Report CheckpointRuntime::write(const CrawlCheckpoint& snapshot)
{
    std::lock_guard lock{writer_};
    if (snapshot.sequence <= last_written_) { return {}; }
    const auto started = HostScheduler::Clock::now();
    try {
        Report report;
        if (snapshot.spool_sync) { snapshot.spool_sync->sync(); }
        if (!options_.checkpoint.path.empty()) { report.bytes = CheckpointManager::write(options_.checkpoint.path, snapshot); }
        if (options_.checkpoint.on_snapshot) { options_.checkpoint.on_snapshot(snapshot); }
        last_written_ = snapshot.sequence;
        report.elapsed = HostScheduler::Clock::now() - started;
        return report;
    } catch (const std::exception& error) {
        std::cerr << "Checkpoint failed: " << error.what() << '\n';
        return {0, error.what(), HostScheduler::Clock::now() - started};
    }
}
void CheckpointRuntime::report(CrawlResult& result, Report report)
{
    result.checkpoint_time += report.elapsed;
    if (!report.error.empty()) { result.checkpoint_errors.push_back(std::move(report.error)); }
    else if (report.bytes != 0) { ++result.checkpoints_written; result.checkpoint_bytes = report.bytes; }
}
} // namespace fluxcrawl::detail
