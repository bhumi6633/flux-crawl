#include <fluxcrawl/checkpoint.hpp>
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
        if (argc < 2) { throw std::invalid_argument("Usage: persistence_scale OUTPUT_DIRECTORY [counts...]"); }
        const std::filesystem::path directory{argv[1]}; std::filesystem::create_directories(directory);
        std::vector<std::size_t> counts{1000, 10000, 100000};
        if (argc > 2) { counts.clear(); for (int i = 2; i < argc; ++i) { counts.push_back(std::stoull(argv[i])); } }
        std::cout << "scenario,seen,pending,run,bytes,write_ms,load_ms\n" << std::fixed << std::setprecision(3);
        for (const auto count : counts) {
            auto c = synthetic(count);
            const auto path = directory / ("snapshot-" + std::to_string(count) + ".chk");
            for (int run = 1; run <= 3; ++run) {
                ++c.sequence;
                auto begin = Clock::now(); const auto bytes = CheckpointManager::write(path, c); const auto write = Clock::now() - begin;
                begin = Clock::now(); const auto restored = CheckpointManager::read(path); const auto load = Clock::now() - begin;
                if (restored.seen.size() != count || restored.unfinished.size() != c.unfinished.size()) { throw std::runtime_error("Scaling sample failed validation"); }
                std::cout << "snapshot," << count << ',' << c.unfinished.size() << ',' << run << ',' << bytes << ',' << ms(write) << ',' << ms(load) << '\n' << std::flush;
            }
            std::filesystem::remove(path);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
