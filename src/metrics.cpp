#include <fluxcrawl/metrics.hpp>
#include <algorithm>
#include <stdexcept>

namespace fluxcrawl {

LatencyStats latency_stats(std::span<const std::chrono::nanoseconds> observations)
{
    LatencyStats result;
    result.samples = observations.size();
    if (observations.empty()) { return result; }
    std::vector<std::chrono::nanoseconds> sorted{observations.begin(), observations.end()};
    std::sort(sorted.begin(), sorted.end());
    if (sorted.front().count() < 0) { throw std::invalid_argument("Negative request latency"); }
    const auto rank = [&](std::size_t percentile) {
        // Avoid N * percentile overflow without floating-point index rounding.
        const auto index = (sorted.size() / 100) * percentile
            + ((sorted.size() % 100) * percentile + 99) / 100 - 1;
        return sorted[index];
    };
    long double total_ns = 0;
    for (const auto value : sorted) { total_ns += static_cast<long double>(value.count()); }
    result.mean_ms = static_cast<double>(total_ns / static_cast<long double>(sorted.size()) / 1000000.0L);
    result.minimum = sorted.front();
    result.maximum = sorted.back();
    result.p50 = rank(50);
    result.p95 = rank(95);
    result.p99 = rank(99);
    return result;
}

CrawlMetrics crawl_metrics(const CrawlResult& result)
{
    CrawlMetrics metrics;
    const double seconds = std::chrono::duration<double>(result.elapsed).count();
    if (seconds > 0) {
        metrics.attempts_per_second = static_cast<double>(result.pages_attempted - result.session_start_attempts) / seconds;
        metrics.responses_per_second = static_cast<double>(result.responses_received - result.session_start_responses) / seconds;
        metrics.bytes_per_second = static_cast<double>(result.bytes_downloaded - result.session_start_bytes) / seconds;
    }
    std::vector<std::chrono::nanoseconds> observations;
    observations.reserve(result.records.size());
    for (const auto& record : result.records) { observations.push_back(record.latency); }
    metrics.latency = latency_stats(observations);
    return metrics;
}

} // namespace fluxcrawl
