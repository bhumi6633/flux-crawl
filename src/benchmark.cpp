#include <fluxcrawl/benchmark.hpp>
#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/version.hpp>
#include "benchmark_server_client.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace fluxcrawl {
std::string_view engine_name(EngineType engine)
{
    switch (engine) {
    case EngineType::Sequential: return "sequential";
    case EngineType::Threaded: return "threaded";
    case EngineType::Async: return "async";
    }
    throw std::invalid_argument("Unknown benchmark engine");
}
namespace {
CrawlResult crawl(EngineType engine, std::size_t concurrency, const BenchmarkConfig& config)
{
    const auto http = detail::benchmark_http_options();
    auto options = config.crawl;
    options.same_host_only = true;
    switch (engine) {
    case EngineType::Sequential: return SequentialCrawler{http}.crawl(config.seed, options);
    case EngineType::Threaded: return ThreadPoolCrawler{concurrency, http}.crawl(config.seed, options);
    case EngineType::Async: return AsyncCrawler{concurrency, http}.crawl(config.seed, options);
    }
    throw std::invalid_argument("Unknown benchmark engine");
}
void validate(const CrawlResult& result, const BenchmarkConfig& config, const BenchmarkWorkload& workload,
              const std::vector<std::size_t>& depths, std::size_t expected)
{
    const auto fail = [&] {
        std::string reason = "Invalid benchmark sample: incomplete or different logical workload; expected="
            + std::to_string(expected) + " attempted=" + std::to_string(result.pages_attempted)
            + " responses=" + std::to_string(result.responses_received) + " failures=" + std::to_string(result.fetch_failures)
            + " bytes=" + std::to_string(result.bytes_downloaded) + " discovered=" + std::to_string(result.urls_discovered)
            + " pending=" + std::to_string(result.urls_pending);
        for (const auto& record : result.records) {
            if (const auto* error = std::get_if<HttpError>(&record.outcome)) {
                reason += "; first HTTP error: " + error->message;
                break;
            }
        }
        throw std::runtime_error(reason);
    };
    if (result.pages_attempted != expected || result.responses_received != expected || result.fetch_failures != 0
        || result.records.size() != expected || result.bytes_downloaded != expected * workload.body_bytes
        || result.urls_pending != (expected == 0 ? 1U : 0U)
        || result.urls_discovered != (expected == 0 ? 1U : expected)) { fail(); }
    std::vector<bool> seen(expected, false);
    for (const auto& record : result.records) {
        const std::string_view path = record.item.url.path();
        if (!path.starts_with("/page/")) { fail(); }
        const auto id = path.substr(6);
        std::size_t page = 0;
        const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), page);
        if (error != std::errc{} || end != id.data() + id.size() || page >= expected || seen[page]) { fail(); }
        seen[page] = true;
        const auto expected_url = Url::resolve(config.seed, "/page/" + std::to_string(page)).value();
        const auto* response = std::get_if<CrawlResponseMetadata>(&record.outcome);
        if (record.item.url != expected_url || record.item.depth != depths[page] || !response
            || response->effective_url != expected_url || response->status_code != 200
            || !is_html_content_type(response->content_type) || response->downloaded_bytes != workload.body_bytes
            || record.latency.count() < 0 || record.latency > result.elapsed) { fail(); }
    }
}
double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : values[middle - 1] / 2 + values[middle] / 2;
}
} // namespace

std::vector<BenchmarkAggregate> aggregate_samples(std::span<const BenchmarkSample> samples)
{
    std::map<std::pair<EngineType, std::size_t>, std::vector<const BenchmarkSample*>> groups;
    for (const auto& sample : samples) { groups[{sample.engine, sample.concurrency}].push_back(&sample); }
    std::vector<BenchmarkAggregate> result;
    for (const auto& [key, group] : groups) {
        std::vector<double> elapsed, rates;
        for (const auto* sample : group) {
            elapsed.push_back(std::chrono::duration<double, std::milli>(sample->elapsed).count());
            if (sample->metrics.attempts_per_second) { rates.push_back(*sample->metrics.attempts_per_second); }
        }
        // Welford's update avoids subtracting large, nearly equal squared sums.
        double mean = 0, m2 = 0;
        std::size_t count = 0;
        for (const double value : elapsed) {
            ++count;
            const double delta = value - mean;
            mean += delta / static_cast<double>(count);
            m2 += delta * (value - mean);
        }
        const double deviation = std::sqrt(std::max(0.0, m2 / static_cast<double>(count)));
        result.push_back({key.first, key.second, group.size(), median(std::move(elapsed)),
                          rates.empty() ? std::nullopt : std::optional{median(std::move(rates))}, deviation});
    }
    return result;
}

BenchmarkReport BenchmarkRunner::run(const BenchmarkConfig& config) const
{
    if (config.crawl.scheduling.max_per_host || config.crawl.scheduling.rate) {
        throw std::invalid_argument("The single-origin benchmark requires disabled host limits; use the multi-host scenario for scheduling experiments");
    }
    if (config.runs == 0 || config.engines.empty() || config.concurrency.empty()
        || std::find(config.concurrency.begin(), config.concurrency.end(), 0) != config.concurrency.end()) {
        throw std::invalid_argument("Benchmark requires positive runs/concurrency and nonempty engine/concurrency lists");
    }
    if (config.runs > 10000 || config.warmups > 10000) {
        throw std::invalid_argument("Benchmark runs and warmups must not exceed 10000");
    }
    auto engines = config.engines;
    auto levels = config.concurrency;
    for (const auto engine : engines) { (void)engine_name(engine); }
    std::sort(engines.begin(), engines.end());
    engines.erase(std::unique(engines.begin(), engines.end()), engines.end());
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    auto workers = config.workers.empty() ? levels : config.workers;
    std::sort(workers.begin(), workers.end());
    workers.erase(std::unique(workers.begin(), workers.end()), workers.end());
    if (!config.workers.empty() && std::find(engines.begin(), engines.end(), EngineType::Threaded) == engines.end()) {
        throw std::invalid_argument("Benchmark --workers requires the threaded engine");
    }
    std::vector<std::pair<EngineType, std::size_t>> configurations;
    for (const auto engine : engines) {
        if (engine == EngineType::Sequential) { configurations.emplace_back(engine, 1); }
        else {
            const auto& selected = engine == EngineType::Threaded ? workers : levels;
            for (const auto level : selected) {
                const std::size_t cap = engine == EngineType::Threaded ? 256 : 1024;
                if (level == 0 || level > cap) {
                    throw std::invalid_argument("Benchmark limits: workers 1..256, async concurrency 1..1024");
                }
                configurations.emplace_back(engine, level);
            }
        }
    }
    if (config.runs > std::numeric_limits<std::size_t>::max() / configurations.size()) {
        throw std::invalid_argument("Benchmark run count is too large");
    }
    detail::BenchmarkServerClient server{config.seed};
    BenchmarkReport report;
    report.started_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    report.measured_runs = config.runs;
    report.seed = config.seed.canonical();
    report.workload = server.workload();
    if (!config.scenario.empty() && config.scenario != report.workload.scenario) {
        throw std::invalid_argument("Benchmark scenario does not match server metadata");
    }
    report.max_depth = config.crawl.max_depth;
    report.max_pages = config.crawl.max_pages;
    report.warmups = config.warmups;
    report.version = version;
    report.build_type = FLUXCRAWL_BUILD_TYPE;
    if (report.build_type.empty()) { report.build_type = "unspecified"; }
    std::vector<std::size_t> depths(report.workload.pages, 0);
    for (std::size_t page = 1; page < depths.size(); ++page) {
        depths[page] = depths[(page - 1) / report.workload.fanout] + 1;
    }
    report.expected_pages = static_cast<std::size_t>(std::count_if(depths.begin(), depths.end(),
        [&](std::size_t depth) { return depth <= config.crawl.max_depth; }));
    if (config.crawl.max_pages == 0) { report.expected_pages = 0; }
    else if (config.crawl.max_pages < report.expected_pages) {
        throw std::invalid_argument("Benchmark max-pages must cover all pages reachable at the selected depth (or be zero)");
    }
    const auto trial = [&](EngineType engine, std::size_t concurrency, std::size_t run) {
        server.reset();
        auto result = crawl(engine, concurrency, config);
        const auto stats = server.stats();
        try {
            validate(result, config, report.workload, depths, report.expected_pages);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string{error.what()} + "; engine=" + std::string{engine_name(engine)}
                + " concurrency=" + std::to_string(concurrency) + " run=" + std::to_string(run));
        }
        if (stats.active != 0 || stats.requests != report.expected_pages || stats.unique != report.expected_pages
            || stats.peak > concurrency || (report.expected_pages != 0 && stats.peak == 0)) {
            throw std::runtime_error("Invalid benchmark sample: server request counts or concurrency mismatch");
        }
        return BenchmarkSample{engine, concurrency, run, result.pages_attempted, result.responses_received,
            result.fetch_failures, result.bytes_downloaded, result.elapsed, crawl_metrics(result), stats.requests, stats.peak, std::move(result.profile)};
    };
    for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
        for (const auto& [engine, concurrency] : configurations) { (void)trial(engine, concurrency, 0); }
    }
    // Repetition-major order reduces long grouped runs while keeping output deterministic.
    for (std::size_t run = 0; run < config.runs; ++run) {
        for (const auto& [engine, concurrency] : configurations) { report.samples.push_back(trial(engine, concurrency, run + 1)); }
    }
    report.aggregates = aggregate_samples(report.samples);
    return report;
}
} // namespace fluxcrawl
