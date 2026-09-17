#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/html_parser.hpp>

#include "crawl_processing.hpp"

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

    std::optional<CrawlItem> acquire()
    {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [&] {
            return fatal_ || result_.pages_attempted == options_.max_pages
                || active_ == 0 || (ready_ && ready_->depth == active_depth_);
        });
        if (fatal_ || result_.pages_attempted == options_.max_pages || !ready_) {
            return std::nullopt;
        }
        auto item = std::move(ready_);
        ready_ = state_.next();
        active_depth_ = item->depth;
        // Budget reservation and active work transition are atomic with the pop.
        ++result_.pages_attempted;
        ++active_;
        return item;
    }

    void complete(detail::ProcessedPage page)
    {
        {
            std::lock_guard lock{mutex_};
            if (!fatal_) {
                // The seen check, insertion, enqueue, and result update share this lock.
                detail::publish_page(state_, result_, std::move(page), seed_, options_);
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

    CrawlResult finish()
    {
        // Called only after all workers have joined.
        if (fatal_) { std::rethrow_exception(fatal_); }
        result_.urls_discovered = state_.seen_count();
        result_.urls_pending = state_.frontier_size() + (ready_ ? 1U : 0U);
        return std::move(result_);
    }

private:
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
    if (options.max_pages == 0) {
        auto result = coordinator.finish();
        result.elapsed = std::chrono::steady_clock::now() - started;
        return result;
    }
    std::vector<std::thread> workers;
    workers.reserve(workers_);
    try {
        for (std::size_t index = 0; index < workers_; ++index) {
            workers.emplace_back([&] {
                bool claimed = false;
                try {
                    // Immutable options are shared; each fetch owns its easy handle and buffers.
                    while (auto item = coordinator.acquire()) {
                        claimed = true;
                        auto page = detail::process_page(client_, std::move(*item), options);
                        coordinator.complete(std::move(page));
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
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

} // namespace fluxcrawl
