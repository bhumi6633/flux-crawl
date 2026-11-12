#include <fluxcrawl/sequential_crawler.hpp>

#include <fluxcrawl/crawl_state.hpp>
#include "crawl_processing.hpp"
#include "profile_timer.hpp"
#include "persistence_runtime.hpp"
#include <fluxcrawl/observability.hpp>
#include "observability_sampler.hpp"

#include <utility>
#include <thread>
#include <stdexcept>

namespace fluxcrawl {

SequentialCrawler::SequentialCrawler(HttpClientOptions http_options) : client_{std::move(http_options)} {}

CrawlResult SequentialCrawler::crawl(const Url& seed, const CrawlOptions& options) const
{
    options.retry.validate(); options.backpressure.validate();
    const auto started = std::chrono::steady_clock::now();
    if (options.observability) { options.observability->begin("sequential", 1); }
    CrawlState state{seed, options.max_depth, options.backpressure, options.dedup};
    HostScheduler scheduler{1, options.scheduling, detail::persistence_enabled(options), bool(options.resume_from), bool(options.observability)};
    CrawlPolicy policy{options.respect_robots};
    CrawlResult result;
    detail::PersistenceRuntime checkpoints{seed, options, client_.options()};
    checkpoints.initialize(state, scheduler, policy, result);
    const auto observe = [&](bool final = false) {
        if (options.observability) { options.observability->capture(state, scheduler, policy, result, options, final, checkpoints.pending_mutations()); }
    };
    observe();
    std::mutex telemetry_mutex;
    std::unique_ptr<detail::ObservabilitySampler> sampler;
    if (options.observability) {
        sampler=std::make_unique<detail::ObservabilitySampler>(options.observability->interval(), [&] {
            std::lock_guard lock{telemetry_mutex}; observe();
        });
    }
    if (options.profile) { result.profile.emplace(); }
    auto* profile = result.profile ? &result.profile->common : nullptr;
    while ((scheduler.pending() != 0 || state.has_deferred()) && result.pages_attempted < options.max_pages && !checkpoints.stop(result)) {
        std::unique_lock lock{telemetry_mutex, std::defer_lock};
        if (options.observability) { lock.lock(); }
        policy.schedule(state, scheduler, result, profile);
        observe();
        if (auto snapshot = checkpoints.capture(state, scheduler, policy, result)) { detail::PersistenceRuntime::report(result, checkpoints.write(*snapshot)); }
        auto item = [&] {
            detail::ProfileTimer timer{profile ? &profile->frontier : nullptr};
            return scheduler.acquire(HostScheduler::Clock::now());
        }();
        if (!item) {
            if (const auto deadline = scheduler.next_deadline(HostScheduler::Clock::now())) {
                if (lock.owns_lock()) { lock.unlock(); }
                std::this_thread::sleep_until(options.observability ? std::min(*deadline, HostScheduler::Clock::now()+options.observability->interval()) : *deadline);
                continue;
            }
            if (state.has_deferred()) { continue; }
            throw std::logic_error("Pending sequential work has no scheduling deadline");
        }
        detail::reserve_attempt(result, item->item);
        observe();
        if (lock.owns_lock()) { lock.unlock(); }
        auto page=detail::process_page(client_, std::move(item->item), options, profile);
        if (options.observability) { lock.lock(); }
        detail::publish_page(state, result, std::move(page), seed, options, scheduler, policy, profile);
        policy.schedule(state, scheduler, result, profile);
        observe();
        item.reset();
        if (auto snapshot = checkpoints.capture(state, scheduler, policy, result)) {
            detail::PersistenceRuntime::report(result, checkpoints.write(*snapshot));
        }
    }
    sampler.reset();
    result.robots_retries_cancelled = result.robots_retries_scheduled - result.robots_retries_attempted;
    result.retries_cancelled = result.retries_scheduled + result.recovery_retry_attempts - result.retries_attempted;
    result.pressure = state.pressure_metrics();
    result.urls_discovered = state.seen_count();
    result.urls_pending = scheduler.resource_pending() + policy.held();
    if (auto snapshot = checkpoints.capture(state, scheduler, policy, result, true)) {
        detail::PersistenceRuntime::report(result, checkpoints.write(*snapshot));
    }
    result.pressure = state.pressure_metrics();
    result.dedup = state.dedup_metrics();
    result.elapsed = std::chrono::steady_clock::now() - started;
    observe(true);
    return result;
}

} // namespace fluxcrawl
