#include <fluxcrawl/async_crawler.hpp>
#include "crawl_processing.hpp"
#include "profile_timer.hpp"
#include <algorithm>
#include "curl_transfer.hpp"

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace fluxcrawl {
namespace {

void check_multi(CURLMcode code)
{
    if (code != CURLM_OK) {
        throw std::runtime_error(std::string{"Async curl multi failure: "} + curl_multi_strerror(code));
    }
}

struct ActiveTransfer {
    ActiveTransfer(CrawlItem pending, const HttpClientOptions& options)
        : item{std::move(pending)}, request{item.url, options} {}
    CrawlItem item;
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
    const auto started = std::chrono::steady_clock::now();
    CrawlState state{seed, options.max_depth};
    CrawlResult result;
    if (options.profile) { result.profile.emplace(); result.profile->async.emplace(); }
    auto* common = result.profile ? &result.profile->common : nullptr;
    auto* profile = result.profile ? &*result.profile->async : nullptr;
    const auto next = [&] {
        detail::ProfileTimer timer{common ? &common->frontier : nullptr};
        return state.next();
    };
    auto ready = next(); // Lookahead stays pending until committed to an attempt.
    std::size_t active_depth = 0;
    MultiTransfers transfers;

    const auto publish = [&](HttpResult response, CrawlItem item, std::chrono::nanoseconds latency) {
        detail::publish_page(state, result,
            detail::process_response(std::move(response), std::move(item), options, latency, common), seed, options, common);
        if (!ready) { ready = next(); }
    };
    const auto fill = [&] {
        detail::ProfileTimer timer{profile ? &profile->refill_time : nullptr};
        bool added = false;
        while (ready && transfers.active.size() < concurrency_ && result.pages_attempted < options.max_pages
               && (transfers.active.empty() || ready->depth == active_depth)) {
            auto item = std::move(*ready);
            ready = next();
            active_depth = item.depth;
            ++result.pages_attempted;
            const auto setup_started = std::chrono::steady_clock::now();
            auto transfer = std::make_unique<ActiveTransfer>(std::move(item), http_options_);
            if (transfer->request.setup != CURLE_OK) {
                publish(transfer->request.finish(transfer->request.setup), std::move(transfer->item),
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
    while (!transfers.active.empty()) {
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
                publish(transfer->request.finish(code), std::move(transfer->item), completed - transfer->started);
            }
        }
        // Refill after draining completions, before waiting on any remaining siblings.
        // Preserve BFS depth barriers, but never wait for a whole same-depth batch.
        if (fill()) { continue; } // New handles need perform before the next poll.
        if (!transfers.active.empty()) {
            // poll honors shorter internal curl timers and sleeps even with no descriptors.
            detail::ProfileTimer timer{profile ? &profile->multi_wait : nullptr};
            if (profile) { ++profile->waits; }
            check_multi(curl_multi_poll(transfers.handle, nullptr, 0, 1000, nullptr));
        }
    }
    transfers.close();
    result.urls_discovered = state.seen_count();
    result.urls_pending = state.frontier_size() + (ready ? 1U : 0U);
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

} // namespace fluxcrawl
