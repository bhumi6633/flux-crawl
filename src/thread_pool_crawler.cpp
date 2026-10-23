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
    Coordinator(const Url& seed, const CrawlOptions& options)
        : seed_{seed}, options_{options}, state_{seed, options.max_depth}, ready_{state_.next()} {}

    std::optional<CrawlItem> acquire(WorkerProfile* profile)
    {
        auto lock = acquire_lock(profile);
        const auto runnable = [&] {
            return fatal_ || result_.pages_attempted == options_.max_pages
                || active_ == 0 || (ready_ && ready_->depth == active_depth_);
        };
        while (!runnable()) {
            detail::ProfileTimer timer{profile ? &profile->idle : nullptr};
            if (profile) { ++profile->waits; }
            changed_.wait(lock);
            if (profile) { ++profile->wakeups; }
        }
        detail::ProfileTimer timer{profile ? &profile->common.frontier : nullptr};
        if (fatal_ || result_.pages_attempted == options_.max_pages || !ready_) {
            return std::nullopt;
        }
        auto item = std::move(ready_);
        ready_ = state_.next();
        active_depth_ = item->depth;
        // Budget reservation and active work transition are atomic with the pop.
        ++result_.pages_attempted;
        ++active_;
        if (profile) { max_active_ = std::max(max_active_, active_); }
        return item;
    }

    void complete(detail::ProcessedPage page, WorkerProfile* profile)
    {
        {
            auto lock = acquire_lock(profile);
            // Frontier includes publication bookkeeping and discovery under the lock.
            detail::ProfileTimer timer{profile ? &profile->common.frontier : nullptr};
            if (!fatal_) {
                // The seen check, insertion, enqueue, and result update share this lock.
                detail::publish_page(state_, result_, std::move(page), seed_, options_, profile ? &profile->common : nullptr);
                if (!ready_) { ready_ = state_.next(); }
            }
            --active_;
        }
        // Multiple new items or a depth transition can make every waiter runnable.
        changed_.notify_all();
    }

    void fail(std::exception_ptr error, bool claimed)
    {
        {
            std::lock_guard lock{mutex_};
            if (!fatal_) { fatal_ = std::move(error); }
            if (claimed) { --active_; }
        }
        changed_.notify_all();
    }

    std::size_t max_active() const noexcept { return max_active_; }

    CrawlResult finish()
    {
        // Called only after all workers have joined.
        if (fatal_) { std::rethrow_exception(fatal_); }
        result_.urls_discovered = state_.seen_count();
        result_.urls_pending = state_.frontier_size() + (ready_ ? 1U : 0U);
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
    // One lookahead item remains queued, not active/attempted, until acquire().
    std::optional<CrawlItem> ready_;
    CrawlResult result_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::exception_ptr fatal_;
    // Active means claimed but not published (or failed). Empty queue alone is not done.
    std::size_t active_ = 0;
    std::size_t active_depth_ = 0;
};

} // namespace

ThreadPoolCrawler::ThreadPoolCrawler(std::size_t workers, HttpClientOptions http_options)
    : workers_{workers}, client_{std::move(http_options)}
{
    if (workers == 0) { throw std::invalid_argument("workers must be greater than zero"); }
}

CrawlResult ThreadPoolCrawler::crawl(const Url& seed, const CrawlOptions& options) const
{
    const auto started = std::chrono::steady_clock::now();
    // Initialize libxml2 on the calling thread. HttpClient already initialized curl.
    const HtmlLinkExtractor initialized_parser;
    (void)initialized_parser;
    Coordinator coordinator{seed, options};
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
                bool claimed = false;
                try {
                    // Immutable options are shared; each fetch owns its easy handle and buffers.
                    while (auto item = coordinator.acquire(worker_profile)) {
                        detail::ProfileTimer activity{worker_profile ? &worker_profile->activity : nullptr};
                        claimed = true;
                        if (worker_profile) { ++worker_profile->requests; }
                        auto page = detail::process_page(client_, std::move(*item), options,
                            worker_profile ? &worker_profile->common : nullptr);
                        coordinator.complete(std::move(page), worker_profile);
                        claimed = false;
                    }
                } catch (...) {
                    coordinator.fail(std::current_exception(), claimed);
                }
            });
        }
    } catch (...) {
        // A partially created pool must be woken and joined too.
        coordinator.fail(std::current_exception(), false);
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
