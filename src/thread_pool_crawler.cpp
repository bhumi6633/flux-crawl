#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/html_parser.hpp>

#include "crawl_processing.hpp"
#include "profile_timer.hpp"
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
    Coordinator(const Url& seed, const CrawlOptions& options, std::size_t workers)
        : seed_{seed}, options_{options}, state_{seed, options.max_depth}, scheduler_{workers, options.scheduling}
    { state_.schedule_pending(scheduler_); }

    std::optional<HostScheduler::Lease> acquire(WorkerProfile* profile)
    {
        auto lock = acquire_lock(profile);
        for (;;) {
            if (fatal_ || result_.pages_attempted == options_.max_pages) { return std::nullopt; }
            {
                detail::ProfileTimer timer{profile ? &profile->common.frontier : nullptr};
                if (auto claim = scheduler_.acquire(HostScheduler::Clock::now())) {
                    ++result_.pages_attempted;
                    if (profile) { max_active_ = std::max(max_active_, scheduler_.active()); }
                    return claim;
                }
            }
            if (scheduler_.pending() == 0 && scheduler_.active() == 0) { return std::nullopt; }
            const auto deadline = scheduler_.next_deadline(HostScheduler::Clock::now());
            detail::ProfileTimer timer{profile ? &profile->idle : nullptr};
            if (profile) { ++profile->waits; }
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
                detail::publish_page(state_, result_, std::move(page), seed_, options_, scheduler_, profile ? &profile->common : nullptr);
                state_.schedule_pending(scheduler_);
            }
            claim.release();
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
        result_.retries_cancelled = result_.retries_scheduled - result_.retries_attempted;
        result_.urls_discovered = state_.seen_count();
        result_.urls_pending = scheduler_.pending();
        return std::move(result_);
    }

private:
    std::unique_lock<std::mutex> acquire_lock(WorkerProfile* profile)
    {
        std::unique_lock lock{mutex_, std::defer_lock};
        {
            detail::ProfileTimer timer{profile ? &profile->lock_wait : nullptr};
            lock.lock();
        }
        if (profile) { ++profile->lock_acquisitions; }
        return lock;
    }
    std::size_t max_active_ = 0;
    const Url& seed_;
    const CrawlOptions& options_;
    CrawlState state_;
    HostScheduler scheduler_;
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
    options.retry.validate();
    const auto started = std::chrono::steady_clock::now();
    // Initialize libxml2 on the calling thread. HttpClient already initialized curl.
    const HtmlLinkExtractor initialized_parser;
    (void)initialized_parser;
    Coordinator coordinator{seed, options, workers_};
    std::optional<ThreadProfile> profile;
    if (options.profile) { profile.emplace(); }
    if (options.max_pages == 0) {
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
