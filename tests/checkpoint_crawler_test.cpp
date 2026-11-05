#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace fluxcrawl;
int main(int argc, char** argv)
{
    if (argc != 10) { return 2; }
    try {
        const std::string mode{argv[1]}, engine{argv[3]}, capture_mode{argv[9]};
        auto seed = mode == "resume" ? CheckpointManager::read(argv[2]).seed : Url::parse(argv[2]).value();
        CrawlOptions options; HttpClientOptions http;
        http.use_environment_proxy = false;
        options.max_depth = 3; options.max_pages = std::stoull(argv[6]);
        options.respect_robots = std::string{argv[7]} == "1";
        options.retry = {2, std::chrono::milliseconds{200}, std::chrono::milliseconds{1000}, 2, false};
        options.scheduling.max_per_host = 2;
        if (mode == "resume") {
            auto saved = std::make_shared<CrawlCheckpoint>(CheckpointManager::read(argv[2]));
            options = saved->options; options.resume_from = saved; http = saved->http;
        }
        options.checkpoint.path = argv[4]; options.checkpoint.every = 1;
        if (std::string{argv[5]} != "none") { options.checkpoint.stop_after_attempts = std::stoull(argv[5]); }
        if (capture_mode == "active") {
            const std::filesystem::path destination{std::string{argv[4]} + ".active"};
            options.checkpoint.on_snapshot = [destination](const CrawlCheckpoint& c) {
                if (!std::filesystem::exists(destination) && c.counters.pages_attempted >= 3
                    && std::any_of(c.unfinished.begin(), c.unfinished.end(), [](const auto& w) { return w.was_active; })) {
                    CheckpointManager::write(destination, c);
                }
            };
        }
        std::chrono::nanoseconds injected_pause{};
        if (capture_mode == "pause") {
            options.max_depth = 0;
            options.checkpoint.on_snapshot = [&](const CrawlCheckpoint&) {
                if (injected_pause.count() == 0) {
                    const auto began = HostScheduler::Clock::now();
                    std::this_thread::sleep_for(std::chrono::milliseconds{250});
                    injected_pause = HostScheduler::Clock::now() - began;
                }
            };
        }
        options.profile = true;
        const auto concurrency = std::stoull(argv[8]);
        const auto result = [&] {
            if (engine == "sequential") { return SequentialCrawler{http}.crawl(seed, options); }
            if (engine == "threaded") { return ThreadPoolCrawler{concurrency, http}.crawl(seed, options); }
            return AsyncCrawler{concurrency, http}.crawl(seed, options);
        }();
        if (capture_mode == "pause" && !(result.records.front().latency + injected_pause * 4 / 5 < result.elapsed)) {
            throw std::runtime_error("Checkpoint pause contaminated async latency");
        }
        if (result.pages_attempted != result.responses_received + result.fetch_failures + result.attempts_unresolved
            || result.resources_completed + result.urls_pending + result.urls_blocked_by_robots + result.robots_resources_reused != result.urls_discovered
            || !result.checkpoint_errors.empty()) { throw std::runtime_error("Inconsistent cumulative accounting or checkpoint write failure"); }
        for (const auto& record : result.records) {
            std::cout << "record\t" << record.item.url.str() << '\t' << record.item.depth << '\t' << record.item.retry_count << '\t' << record.item.recovery << '\n';
        }
        const auto saved = CheckpointManager::read(options.checkpoint.path);
        if (saved.counters.pages_attempted != result.pages_attempted) { throw std::runtime_error("Final checkpoint stale"); }
        for (const auto& u : saved.seen) { std::cout << "seen\t" << u.str() << '\n'; }
        std::cout << "summary\t" << result.pages_attempted << '\t' << result.urls_discovered << '\t' << result.urls_pending << '\t'
            << result.resources_completed << '\t' << result.urls_blocked_by_robots << '\t' << result.attempts_unresolved << '\t'
            << result.recovery_attempts << '\t' << result.retries_attempted << '\t' << result.robots_fetches << '\t' << result.checkpoints_written << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
