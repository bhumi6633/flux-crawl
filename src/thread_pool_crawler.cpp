#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/html_parser.hpp>

#include "crawl_processing.hpp"
#include "profile_timer.hpp"
#include "persistence_runtime.hpp"
#include <fluxcrawl/observability.hpp>
#include "observability_sampler.hpp"
#include <algorithm>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fluxcrawl {
namespace {

class Coordinator {
public:
    Coordinator(const Url& seed, const CrawlOptions& options, std::size_t workers, const HttpClientOptions& http)
        : seed_{seed}, options_{options}, state_{seed, options.max_depth, options.backpressure, options.dedup}, scheduler_{workers, options.scheduling, detail::persistence_enabled(options), bool(options.resume_from), bool(options.observability)}, policy_{options.respect_robots}, checkpoints_{seed, options, http}
    {
        if (options_.observability) { options_.observability->begin("threaded", workers); }
        checkpoints_.initialize(state_, scheduler_, policy_, result_);
        observe();
    }
    void observe(bool final = false) {
        if (options_.observability) { options_.observability->capture(state_, scheduler_, policy_, result_, options_, final, checkpoints_.pending_mutations()); }
    }
    void sample() { std::lock_guard lock{mutex_}; observe(); }

    std::optional<HostScheduler::Lease> acquire(WorkerProfile* profile)
    {
        auto lock = acquire_lock(profile);
        for (;;) {
            policy_.schedule(state_, scheduler_, result_, profile ? &profile->common : nullptr);
            // Admission can hit the mutation threshold even when a host/deadline
            // prevents claiming work. Slow durable writes backpressure this worker.
            if (auto snapshot = checkpoints_.capture(state_, scheduler_, policy_, result_)) {
                lock.unlock();
                detail::PersistenceRuntime::Report report;
                try { report = checkpoints_.write(*snapshot); } catch (...) { lock.lock(); throw; }
                lock.lock(); detail::PersistenceRuntime::report(result_, std::move(report));
            }
            observe();
            if (fatal_ || result_.pages_attempted == options_.max_pages || checkpoints_.stop(result_)) { return std::nullopt; }
            {
                auto claim = [&] {
                    detail::ProfileTimer timer{profile ? &profile->common.frontier : nullptr};
                    return scheduler_.acquire(HostScheduler::Clock::now());
                }();
                if (claim) {
                    detail::reserve_attempt(result_, claim->item);
                    observe();
                    if (profile) { max_active_ = std::max(max_active_, scheduler_.active()); }
                    if (auto snapshot = checkpoints_.capture(state_, scheduler_, policy_, result_)) {
                        lock.unlock();
                        detail::PersistenceRuntime::Report report;
                        try { report = checkpoints_.write(*snapshot); }
                        catch (...) { lock.lock(); throw; }
                        lock.lock();
                        detail::PersistenceRuntime::report(result_, std::move(report));
                    }
                    return claim;
                }
            }
            if (scheduler_.pending() == 0 && scheduler_.active() == 0 && !state_.has_deferred()) { return std::nullopt; }
            if (scheduler_.pending() == 0 && scheduler_.active() == 0 && state_.has_deferred()) { continue; }
            const auto deadline = scheduler_.next_deadline(HostScheduler::Clock::now());
            detail::ProfileTimer timer{profile ? &profile->idle : nullptr};
            if (profile) { ++profile->waits; }
            if (options_.observability) { options_.observability->worker_wait(); }
            if (deadline) { changed_.wait_until(lock, *deadline); }
            else { changed_.wait(lock); }
            if (profile) { ++profile->wakeups; }
        }
    }

    void complete(detail::ProcessedPage page, HostScheduler::Lease& claim, WorkerProfile* profile)
    {
        {
            auto lock = acquire_lock(profile);
            // Frontier includes publication bookkeeping and discovery under the lock.
            detail::ProfileTimer timer{profile ? &profile->common.frontier : nullptr};
            if (!fatal_) {
                // The seen check, insertion, enqueue, and result update share this lock.
                detail::publish_page(state_, result_, std::move(page), seed_, options_, scheduler_, policy_, profile ? &profile->common : nullptr);
                policy_.schedule(state_, scheduler_, result_, profile ? &profile->common : nullptr);
            }
            claim.release();
            observe();
        }
        // Multiple new items or a depth transition can make every waiter runnable.
        changed_.notify_all();
    }

    void fail(std::exception_ptr error, HostScheduler::Lease* claim)
    {
        {
            std::lock_guard lock{mutex_};
            if (!fatal_) { fatal_ = std::move(error); }
            if (claim) { claim->release(); }
        }
        changed_.notify_all();
    }

    std::size_t max_active() const noexcept { return max_active_; }

    CrawlResult finish()
    {
        // Called only after all workers have joined.
        if (fatal_) { std::rethrow_exception(fatal_); }
        result_.robots_retries_cancelled = result_.robots_retries_scheduled - result_.robots_retries_attempted;
        result_.retries_cancelled = result_.retries_scheduled + result_.recovery_retry_attempts - result_.retries_attempted;
        result_.pressure = state_.pressure_metrics();
        result_.urls_discovered = state_.seen_count();
        result_.urls_pending = scheduler_.resource_pending() + policy_.held();
        if (auto snapshot = checkpoints_.capture(state_, scheduler_, policy_, result_, true)) {
            detail::PersistenceRuntime::report(result_, checkpoints_.write(*snapshot));
        }
        result_.pressure = state_.pressure_metrics();
        result_.dedup = state_.dedup_metrics();
        observe(true);
        return std::move(result_);
    }

private:
    std::unique_lock<std::mutex> acquire_lock(WorkerProfile* profile)
    {
        std::unique_lock lock{mutex_, std::defer_lock};
        {
            detail::ProfileTimer timer{profile ? &profile->lock_wait : nullptr};
            const auto began = options_.observability ? HostScheduler::Clock::now() : HostScheduler::Time{};
            lock.lock();
            if (options_.observability) { options_.observability->lock_wait(HostScheduler::Clock::now()-began); }
        }
        if (profile) { ++profile->lock_acquisitions; }
        return lock;
    }
    std::size_t max_active_ = 0;
    const Url& seed_;
    const CrawlOptions& options_;
    CrawlState state_;
    HostScheduler scheduler_;
    CrawlPolicy policy_;
    detail::PersistenceRuntime checkpoints_;
    CrawlResult result_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::exception_ptr fatal_;

};

} // namespace

ThreadPoolCrawler::ThreadPoolCrawler(std::size_t workers, HttpClientOptions http_options)
    : workers_{workers}, client_{std::move(http_options)}
{
    if (workers == 0) { throw std::invalid_argument("workers must be greater than zero"); }
}

CrawlResult ThreadPoolCrawler::crawl(const Url& seed, const CrawlOptions& options) const
{
    options.retry.validate(); options.backpressure.validate();
    const auto started = std::chrono::steady_clock::now();
    // Initialize libxml2 on the calling thread. HttpClient already initialized curl.
    const HtmlLinkExtractor initialized_parser;
    (void)initialized_parser;
    Coordinator coordinator{seed, options, workers_, client_.options()};
    std::unique_ptr<detail::ObservabilitySampler> sampler;
    if (options.observability) {
        sampler=std::make_unique<detail::ObservabilitySampler>(options.observability->interval(), [&] { coordinator.sample(); });
    }
    std::optional<ThreadProfile> profile;
    if (options.profile) { profile.emplace(); }
    if (options.max_pages == 0) {
        sampler.reset();
        auto result = coordinator.finish();
        if (profile) { result.profile.emplace(); result.profile->threaded = std::move(profile); }
        result.elapsed = std::chrono::steady_clock::now() - started;
        return result;
    }
    if (profile) { profile->workers.resize(workers_); }
    std::vector<std::thread> workers;
    workers.reserve(workers_);
    try {
        for (std::size_t index = 0; index < workers_; ++index) {
            workers.emplace_back([&, index] {
                auto* worker_profile = profile ? &profile->workers[index] : nullptr;
                std::optional<HostScheduler::Lease> claim;
                try {
                    // Immutable options are shared; each fetch owns its easy handle and buffers.
                    while (auto item = coordinator.acquire(worker_profile)) {
                        detail::ProfileTimer activity{worker_profile ? &worker_profile->activity : nullptr};
                        claim.emplace(std::move(*item));
                        if (worker_profile) { ++worker_profile->requests; }
                        auto page = detail::process_page(client_, std::move(claim->item), options,
                            worker_profile ? &worker_profile->common : nullptr);
                        coordinator.complete(std::move(page), *claim, worker_profile);
                        claim.reset();
                    }
                } catch (...) {
                    coordinator.fail(std::current_exception(), claim ? &*claim : nullptr);
                }
            });
        }
    } catch (...) {
        // A partially created pool must be woken and joined too.
        coordinator.fail(std::current_exception(), nullptr);
    }
    for (auto& worker : workers) { worker.join(); }
    sampler.reset();
    auto result = coordinator.finish();
    if (profile) {
        for (const auto& worker : profile->workers) { profile->total.merge(worker); }
        profile->max_active = coordinator.max_active();
        result.profile.emplace();
        result.profile->common = profile->total.common;
        result.profile->threaded = std::move(profile);
    }
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

} // namespace fluxcrawl
