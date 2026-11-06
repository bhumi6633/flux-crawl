#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include <fluxcrawl/crawl_policy.hpp>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace fluxcrawl;
using Clock = std::chrono::steady_clock;
double ms(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }
CrawlCheckpoint synthetic(std::size_t count)
{
    auto seed = Url::parse("http://127.0.0.1:8080/page/0").value();
    CrawlCheckpoint c{.seed = seed};
    c.options.max_depth = 3; c.options.max_pages = count + 1000;
    const auto pending = std::min(std::size_t{100}, count);
    for (std::size_t i = 0; i < count; ++i) {
        auto url = Url::parse("http://127.0.0.1:8080/page/" + std::to_string(i)).value();
        c.seen.push_back(url);
        if (i >= count - pending) { c.unfinished.push_back({{url, i == 0 ? 0U : 1U}, std::nullopt, false}); }
    }
    auto& r = c.counters;
    r.pages_attempted = r.responses_received = r.resources_completed = count - pending;
    r.urls_discovered = count; r.urls_pending = pending;
    return c;
}
int main(int argc, char** argv)
{
    try {
#ifndef NDEBUG
        throw std::runtime_error("Persistence measurements require a Release build");
#endif
        const bool sqlite = argc > 1 && std::string_view{argv[1]} == "--sqlite";
        const int directory_arg = sqlite ? 2 : 1;
        if (argc <= directory_arg) { throw std::invalid_argument("Usage: persistence_scale [--sqlite] OUTPUT_DIRECTORY [counts...]"); }
        const std::filesystem::path directory{argv[directory_arg]}; std::filesystem::create_directories(directory);
        std::vector<std::size_t> counts{1000, 10000, 100000};
        if (argc > directory_arg + 1) { counts.clear(); for (int i = directory_arg + 1; i < argc; ++i) { counts.push_back(std::stoull(argv[i])); } }
        std::cout << (sqlite ? "scenario,seen,pending,run,sqlite_version,initialization_ms,incremental_ms,mutations,transactions,bytes,close_ms,open_ms,seen_read_ms,work_read_ms,validation_ms,hot_seen_ms,scheduler_ms,total_startup_ms\n" : "scenario,seen,pending,run,bytes,write_ms,load_ms\n") << std::fixed << std::setprecision(3);
        for (const auto count : counts) {
            auto c = synthetic(count);
            for (int run = 1; run <= 3; ++run) {
                if (!sqlite) {
                    const auto path = directory / ("snapshot-" + std::to_string(count) + ".chk");
                    ++c.sequence;
                    auto begin = Clock::now(); const auto bytes = CheckpointManager::write(path, c); const auto write = Clock::now() - begin;
                    begin = Clock::now(); const auto restored = CheckpointManager::read(path); const auto load = Clock::now() - begin;
                    if (restored.seen.size() != count || restored.unfinished.size() != c.unfinished.size()) { throw std::runtime_error("Scaling sample failed validation"); }
                    std::cout << "snapshot," << count << ',' << c.unfinished.size() << ',' << run << ',' << bytes << ',' << ms(write) << ',' << ms(load) << '\n' << std::flush;
                    std::filesystem::remove(path);
                } else {
                    const auto path = directory / ("sqlite-" + std::to_string(count) + "-" + std::to_string(run) + ".db");
                    auto began = Clock::now(); auto store = SqliteStore::create(path, c); const auto initialization = Clock::now() - began;
                    auto metadata = CheckpointManager::metadata_only(c); ++metadata.sequence; metadata.counters.urls_discovered += 100; metadata.counters.urls_pending += 100;
                    CrawlMutationBatch batch{metadata, {}, {}, 200, 0};
                    for (std::size_t i = 0; i < 100; ++i) {
                        auto url = Url::parse("http://127.0.0.1:8080/page/" + std::to_string(count + i)).value();
                        batch.discoveries.push_back(url); batch.changes.push_back({url, CheckpointWork{{url, 1}, {}, false}, 1000 + i});
                    }
                    began = Clock::now(); store->apply(batch); const auto incremental = Clock::now() - began;
                    const auto stats = store->metrics(); began = Clock::now(); store.reset(); const auto close = Clock::now() - began;
                    const auto bytes = std::filesystem::file_size(path);
                    const auto startup = Clock::now(); began = startup; store = SqliteStore::open(path); const auto open = Clock::now() - began;
                    auto restored = store->load(); const auto timings = store->load_timings();
                    began = Clock::now(); CrawlState hot{restored.seed, restored.options.max_depth}; hot.restore_seen(restored.seen); const auto hot_seen = Clock::now() - began;
                    began = Clock::now(); HostScheduler scheduler{16, restored.options.scheduling, false, true}; CrawlPolicy policy{restored.options.respect_robots}; CrawlResult result;
                    for (const auto& work : restored.unfinished) {
                        policy.accept({work.item, work.eligible_unix_ms ? std::optional{CheckpointManager::steady_deadline(*work.eligible_unix_ms, HostScheduler::Clock::now(), std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())} : std::nullopt}, scheduler, result);
                    }
                    const auto rebuild = Clock::now() - began; const auto total = Clock::now() - startup;
                    if (hot.seen_count() != count + 100 || scheduler.pending() != 200 || stats.transactions != 1) { throw std::runtime_error("SQLite scaling sample validation failed"); }
                    std::cout << "sqlite," << count << ",100," << run << ',' << SqliteStore::library_version() << ',' << ms(initialization) << ',' << ms(incremental)
                        << ',' << stats.mutations << ',' << stats.transactions << ',' << bytes << ',' << ms(close) << ',' << ms(open) << ',' << ms(timings.seen)
                        << ',' << ms(timings.work) << ',' << ms(timings.validation) << ',' << ms(hot_seen) << ',' << ms(rebuild) << ',' << ms(total) << '\n' << std::flush;
                    store.reset(); std::filesystem::remove(path); std::filesystem::remove(path.string() + ".owner");
                }
            }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
