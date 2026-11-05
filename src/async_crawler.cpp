#include <fluxcrawl/async_crawler.hpp>
#include "crawl_processing.hpp"
#include "profile_timer.hpp"
#include "checkpoint_runtime.hpp"
#include <algorithm>
#include "curl_transfer.hpp"

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <thread>

namespace fluxcrawl {
namespace {

void check_multi(CURLMcode code)
{
    if (code != CURLM_OK) {
        throw std::runtime_error(std::string{"Async curl multi failure: "} + curl_multi_strerror(code));
    }
}

struct ActiveTransfer {
    ActiveTransfer(HostScheduler::Lease pending, const HttpClientOptions& options)
        : claim{std::move(pending)}, request{claim.item.url, [&] {
            auto configured = options;
            if (claim.item.policy_request) { configured.max_body_bytes = RobotsPolicy::max_bytes; }
            return configured;
        }()} {}
    HostScheduler::Lease claim;
    detail::CurlTransfer request;
    std::chrono::steady_clock::time_point started;
};

class MultiTransfers {
public:
    MultiTransfers() : handle{curl_multi_init()}
    {
        if (!handle) { throw std::runtime_error("Unable to initialize curl multi handle"); }
    }
    ~MultiTransfers()
    {
        // Exception cleanup cannot throw. Detach every easy handle before freeing it.
        for (const auto& [easy, transfer] : active) {
            (void)transfer;
            (void)curl_multi_remove_handle(handle, easy);
        }
        active.clear();
        if (handle) { (void)curl_multi_cleanup(handle); }
    }
    MultiTransfers(const MultiTransfers&) = delete;
    MultiTransfers& operator=(const MultiTransfers&) = delete;

    void add(std::unique_ptr<ActiveTransfer> transfer)
    {
        auto* easy = transfer->request.handle.get();
        // Allocate the map node BEFORE attaching: a failed allocation owns no attachment.
        active.emplace(easy, std::move(transfer));
        active.at(easy)->started = std::chrono::steady_clock::now();
        const auto code = curl_multi_add_handle(handle, easy);
        if (code != CURLM_OK) {
            active.erase(easy);
            check_multi(code);
        }
    }

    std::unique_ptr<ActiveTransfer> remove(CURL* easy)
    {
        const auto found = active.find(easy);
        if (found == active.end()) { throw std::runtime_error("Unknown completed curl transfer"); }
        check_multi(curl_multi_remove_handle(handle, easy));
        auto transfer = std::move(found->second);
        active.erase(found);
        return transfer;
    }

    void close()
    {
        // Normal cleanup is checked; the destructor is reserved for unwinding.
        auto* finished = std::exchange(handle, nullptr);
        check_multi(curl_multi_cleanup(finished));
    }

    CURLM* handle;
    std::unordered_map<CURL*, std::unique_ptr<ActiveTransfer>> active;
};

} // namespace

AsyncCrawler::AsyncCrawler(std::size_t concurrency, HttpClientOptions http_options)
    : concurrency_{concurrency}, http_options_{std::move(http_options)}
{
    if (concurrency_ == 0) { throw std::invalid_argument("Async concurrency must be greater than zero"); }
    detail::initialize_http(http_options_);
}

CrawlResult AsyncCrawler::crawl(const Url& seed, CrawlOptions options) const
{
    options.retry.validate();
    const auto started = std::chrono::steady_clock::now();
    CrawlState state{seed, options.max_depth};
    HostScheduler scheduler{concurrency_, options.scheduling, detail::checkpointing(options), bool(options.resume_from)};
    CrawlPolicy policy{options.respect_robots};
    CrawlResult result;
    detail::CheckpointRuntime checkpoints{seed, options, http_options_};
    detail::restore_session(seed, options, http_options_, state, scheduler, policy, result);
    if (options.profile) { result.profile.emplace(); result.profile->async.emplace(); }
    auto* common = result.profile ? &result.profile->common : nullptr;
    auto* profile = result.profile ? &*result.profile->async : nullptr;
    MultiTransfers transfers;

    const auto publish = [&](HttpResult response, CrawlItem item, std::chrono::nanoseconds latency) {
        detail::publish_page(state, result,
            detail::process_response(std::move(response), std::move(item), options, latency, common), seed, options, scheduler, policy, common);
        policy.schedule(state, scheduler, result);
    };
    const auto checkpoint = [&](bool final = false) {
        if (auto snapshot = checkpoints.capture(state, scheduler, policy, result, final)) {
            detail::CheckpointRuntime::report(result, checkpoints.write(*snapshot));
        }
    };
    const auto fill = [&] {
        detail::ProfileTimer timer{profile ? &profile->refill_time : nullptr};
        bool added = false;
        while (transfers.active.size() < concurrency_ && result.pages_attempted < options.max_pages && !checkpoints.stop(result)) {
            auto item = [&] {
                detail::ProfileTimer frontier{common ? &common->frontier : nullptr};
                return scheduler.acquire(HostScheduler::Clock::now());
            }();
            if (!item) { break; }
            detail::reserve_attempt(result, item->item);
            const auto setup_started = std::chrono::steady_clock::now();
            auto transfer = std::make_unique<ActiveTransfer>(std::move(*item), http_options_);
            if (transfer->request.setup != CURLE_OK) {
                publish(transfer->request.finish(transfer->request.setup), std::move(transfer->claim.item),
                        std::chrono::steady_clock::now() - setup_started);
            } else {
                transfers.add(std::move(transfer));
                added = true;
                if (profile) {
                    ++profile->refills;
                    profile->max_active = std::max(profile->max_active, transfers.active.size());
                }
            }
        }
        return added;
    };

    (void)fill();
    checkpoint();
    while (!transfers.active.empty() || (scheduler.pending() != 0 && result.pages_attempted < options.max_pages && !checkpoints.stop(result))) {
        if (transfers.active.empty()) {
            if (const auto deadline = scheduler.next_deadline(HostScheduler::Clock::now())) {
                std::this_thread::sleep_until(*deadline);
            }
            (void)fill();
            checkpoint();
            if (transfers.active.empty()) { continue; }
        }
        if (profile) { ++profile->iterations; }
        int running = 0;
        check_multi(curl_multi_perform(transfers.handle, &running));
        {
            detail::ProfileTimer timer{profile ? &profile->completions_time : nullptr};
            int remaining = 0;
            while (auto* message = curl_multi_info_read(transfers.handle, &remaining)) {
                if (message->msg != CURLMSG_DONE) { continue; }
                // The message storage becomes invalid after removal. Copy its result first.
                if (profile) { ++profile->completions; }
                const auto completed = std::chrono::steady_clock::now();
                const auto code = message->data.result;
                auto transfer = transfers.remove(message->easy_handle);
                publish(transfer->request.finish(code), std::move(transfer->claim.item), completed - transfer->started);
            }
        }
        // Refill after draining completions, before waiting on any remaining siblings.
        // Preserve BFS depth barriers, but never wait for a whole same-depth batch.
        const auto added = fill();
        checkpoint();
        if (added) { continue; } // New handles need perform before the next poll.
        if (!transfers.active.empty()) {
            // poll honors shorter internal curl timers and sleeps even with no descriptors.
            detail::ProfileTimer timer{profile ? &profile->multi_wait : nullptr};
            if (profile) { ++profile->waits; }
            int timeout_ms = 1000;
            if (result.pages_attempted < options.max_pages) {
                const auto now = HostScheduler::Clock::now();
                if (const auto deadline = scheduler.next_deadline(now)) {
                    const auto delay = std::chrono::ceil<std::chrono::milliseconds>(*deadline - now).count();
                    timeout_ms = static_cast<int>(std::clamp<decltype(delay)>(delay, 1, 1000));
                }
            }
            check_multi(curl_multi_poll(transfers.handle, nullptr, 0, timeout_ms, nullptr));
        }
    }
    transfers.close();
    result.robots_retries_cancelled = result.robots_retries_scheduled - result.robots_retries_attempted;
    result.retries_cancelled = result.retries_scheduled + result.recovery_retry_attempts - result.retries_attempted;
    result.urls_discovered = state.seen_count();
    result.urls_pending = scheduler.resource_pending() + policy.held();
    checkpoint(true);
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

} // namespace fluxcrawl
