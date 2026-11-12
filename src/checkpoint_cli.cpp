#include "checkpoint_cli.hpp"
#include "persistence_cli.hpp"
#include <fluxcrawl/sqlite_store.hpp>
#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <charconv>
#include <iostream>
#include <stdexcept>

int checkpoint_resume_main(int argc, char* argv[])
{
    try {
        if (argc < 3) { throw std::invalid_argument("Usage: fluxcrawl resume FILE [--engine NAME] [--workers N | --concurrency N]"); }
        const auto restore_began=fluxcrawl::HostScheduler::Clock::now();
        const bool database = fluxcrawl::SqliteStore::is_database(argv[2]);
        auto saved = [&] {
            if (database) { auto store=std::shared_ptr<fluxcrawl::SqliteStore>{fluxcrawl::SqliteStore::open(argv[2])}; auto restored=std::make_shared<fluxcrawl::CrawlCheckpoint>(store->load()); restored->opened_store=std::move(store); return restored; }
            return std::make_shared<fluxcrawl::CrawlCheckpoint>(fluxcrawl::CheckpointManager::read(argv[2]));
        }();
        saved->restore_started_at=restore_began;
        auto options = saved->options;
        options.resume_from = saved;
        if (database) { options.sqlite.path = argv[2]; options.sqlite.resume = true; }
        else { options.checkpoint.path = argv[2]; }
        std::string_view engine = "sequential";
        std::size_t workers = 4, concurrency = 8;
        bool workers_supplied = false, concurrency_supplied = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view flag{argv[i]};
            if (i + 1 >= argc) { throw std::invalid_argument("Missing resume option value"); }
            const std::string_view value{argv[++i]};
            if (flag == "--engine") {
                engine = value;
                if (engine != "sequential" && engine != "threaded" && engine != "async") { throw std::invalid_argument("Invalid resume engine"); }
            } else if (flag == "--checkpoint" && !database) {
                if (value.empty()) { throw std::invalid_argument("Checkpoint path must not be empty"); }
                options.checkpoint.path = value;
            } else if (flag == "--workers" || flag == "--concurrency" || flag == "--checkpoint-every" || flag == "--stop-after-attempts" || flag == "--db-batch-size") {
                std::size_t n = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
                if (value.empty() || error != std::errc{} || end != value.data() + value.size() || (n == 0 && flag != "--stop-after-attempts")) {
                    throw std::invalid_argument("Invalid resume execution option integer");
                }
                if (flag == "--workers") { workers = n; workers_supplied = true; }
                else if (flag == "--concurrency") { concurrency = n; concurrency_supplied = true; }
                else if (flag == "--checkpoint-every" && !database) { options.checkpoint.every = n; }
                else if (flag == "--db-batch-size" && database) { options.sqlite.batch_size = n; }
                else if (flag != "--stop-after-attempts") { throw std::invalid_argument("Persistence controls must match the resumed backend"); }
                else { options.checkpoint.stop_after_attempts = n; }
            } else { throw std::invalid_argument("Resume permits execution options only; semantic configuration is stored in the checkpoint"); }
        }
        if ((workers_supplied && engine != "threaded") || (concurrency_supplied && engine != "async")) {
            throw std::invalid_argument("Workers require threaded engine; concurrency requires async engine");
        }
        if (saved->completed) { std::cout << (database ? "Crawl database already completed; no requests issued.\n" : "Checkpoint already completed; no requests issued.\n"); return 0; }
        if (saved->counters.pages_attempted == options.max_pages) {
            std::cout << "Checkpoint has no remaining page-attempt budget; no requests issued.\n"; return 0;
        }
        const auto result = [&] {
            if (engine == "async") { return fluxcrawl::AsyncCrawler{concurrency, saved->http}.crawl(saved->seed, options); }
            if (engine == "threaded") { return fluxcrawl::ThreadPoolCrawler{workers, saved->http}.crawl(saved->seed, options); }
            return fluxcrawl::SequentialCrawler{saved->http}.crawl(saved->seed, options);
        }();
        std::cout << "Resumed seed: " << saved->seed.str() << "\nEngine: " << engine
            << "\nPages attempted: " << result.pages_attempted << "\nResponses: " << result.responses_received
            << "\nFetch failures: " << result.fetch_failures << "\nURLs discovered: " << result.urls_discovered
            << "\nURLs pending: " << result.urls_pending << "\nRecovery attempts: " << result.recovery_attempts
            << "\nUnresolved prior attempts: " << result.attempts_unresolved
            << "\nRobots fetches: " << result.robots_fetches << "\nCheckpoints written: " << result.checkpoints_written
            << "\nCheckpoint bytes: " << result.checkpoint_bytes
            << "\nCheckpoint I/O time ms: " << std::chrono::duration<double, std::milli>(result.checkpoint_time).count() << '\n';
        if (database) { fluxcrawl::detail::print_database_metrics(std::cout, result); }
        fluxcrawl::detail::print_dedup_metrics(std::cout, result, options.dedup);
        fluxcrawl::detail::print_pressure_metrics(std::cout, result, options.backpressure);
        if (!result.persistence_errors.empty() || !result.checkpoint_errors.empty()) { return 3; }
        return result.persistence_errors.empty() && result.checkpoint_errors.empty() && result.robots_fetch_failures == 0 && result.terminal_fetch_failures == 0
            && (options.retry.max_retries == 0 || result.terminal_http_failures == 0) ? 0 : 1;
    } catch (const std::invalid_argument& error) { std::cerr << "fluxcrawl resume: " << error.what() << '\n'; return 2; }
    catch (const std::exception& error) { std::cerr << "fluxcrawl resume: " << error.what() << '\n'; return 3; }
}
