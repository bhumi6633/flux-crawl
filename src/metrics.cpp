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
        metrics.attempts_per_second = static_cast<double>(result.pages_attempted) / seconds;
        metrics.responses_per_second = static_cast<double>(result.responses_received) / seconds;
        metrics.bytes_per_second = static_cast<double>(result.bytes_downloaded) / seconds;
    }
    std::vector<std::chrono::nanoseconds> observations;
    observations.reserve(result.records.size());
    for (const auto& record : result.records) { observations.push_back(record.latency); }
    metrics.latency = latency_stats(observations);
    return metrics;
}

} // namespace fluxcrawl
