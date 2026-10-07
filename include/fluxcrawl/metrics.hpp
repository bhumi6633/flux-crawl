#pragma once

#include <fluxcrawl/crawl_types.hpp>
#include <optional>
#include <span>

namespace fluxcrawl {

struct LatencyStats {
    std::size_t samples = 0;
    std::optional<std::chrono::nanoseconds> p50, p95, p99;
    std::optional<double> mean_ms{};
    std::optional<std::chrono::nanoseconds> minimum{}, maximum{};
};

// Nearest rank: sorted[ceil(percentile * N / 100) - 1]. Empty input is unavailable.
[[nodiscard]] LatencyStats latency_stats(std::span<const std::chrono::nanoseconds> observations);
struct CrawlMetrics {
    // Rates are unavailable for nonpositive elapsed time, zero for zero counts with positive time.
    std::optional<double> attempts_per_second, responses_per_second, bytes_per_second;
    LatencyStats latency;
};
[[nodiscard]] CrawlMetrics crawl_metrics(const CrawlResult& result);

} // namespace fluxcrawl
