#pragma once

#include <fluxcrawl/metrics.hpp>
#include <ostream>
#include <cstdint>
#include <string_view>

namespace fluxcrawl {

enum class EngineType { Sequential, Threaded, Async };
[[nodiscard]] std::string_view engine_name(EngineType engine);

struct BenchmarkConfig {
    // Must name /page/0 on tools/benchmark_server.py at literal IPv4 loopback.
    Url seed;
    CrawlOptions crawl{.max_depth = 20, .max_pages = 100, .same_host_only = true, .backpressure = {.enabled = false}};
    std::vector<EngineType> engines{EngineType::Sequential, EngineType::Threaded, EngineType::Async};
    std::vector<std::size_t> concurrency{1, 2, 4, 8};
    std::size_t runs = 3;
    std::size_t warmups = 0;
    // Empty worker list preserves the shared concurrency sweep.
    std::vector<std::size_t> workers{};
    std::string scenario{}; // Optional assertion against server metadata.
};
struct BenchmarkWorkload {
    std::size_t pages = 0, fanout = 0, delay_ms = 0, body_bytes = 0;
    std::string scenario = "custom";
    std::size_t shared_anchors = 0;
    std::string delay_pattern = "uniform";
};
struct BenchmarkSample {
    EngineType engine = EngineType::Sequential;
    std::size_t concurrency = 1, run = 1;
    std::size_t pages_attempted = 0, responses_received = 0, fetch_failures = 0, bytes_downloaded = 0;
    std::chrono::nanoseconds elapsed{};
    CrawlMetrics metrics;
    std::size_t server_requests = 0, server_peak_active = 0;
    std::optional<ProfileSummary> profile{};
    QueuePressureMetrics pressure{};
};
struct BenchmarkAggregate {
    EngineType engine;
    std::size_t concurrency, samples;
    double median_elapsed_ms;
    std::optional<double> median_attempts_per_second;
    double elapsed_stddev_ms = 0; // Population standard deviation across measured trials.
};
struct BenchmarkReport {
    std::string seed;
    BenchmarkWorkload workload;
    std::size_t max_depth = 0, max_pages = 0, expected_pages = 0, warmups = 0;
    std::string version, build_type;
    std::size_t measured_runs = 0;
    std::int64_t started_at_unix_ms = 0; // Reproducibility metadata only; never used for durations.
    BackpressureOptions backpressure{.enabled = false};
    std::vector<BenchmarkSample> samples;
    std::vector<BenchmarkAggregate> aggregates;
};

class BenchmarkRunner {
public:
    // Fresh engines/state each trial. Warmups validated but excluded. Invalid workloads throw.
    [[nodiscard]] BenchmarkReport run(const BenchmarkConfig& config) const;
};
[[nodiscard]] std::vector<BenchmarkAggregate> aggregate_samples(std::span<const BenchmarkSample> samples);
void write_benchmark_csv(std::ostream& output, const BenchmarkReport& report);
void write_benchmark_table(std::ostream& output, const BenchmarkReport& report);

} // namespace fluxcrawl
