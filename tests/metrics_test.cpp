#include <fluxcrawl/metrics.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace std::chrono_literals;
int main()
{
    int failures = 0;
    const auto check = [&](bool ok) { if (!ok) { ++failures; } };
    using Duration = std::chrono::nanoseconds;
    check(!fluxcrawl::latency_stats({}).p50);
    check(!fluxcrawl::latency_stats({}).mean_ms);
    const Duration one[]{10ms};
    auto stats = fluxcrawl::latency_stats(one);
    check(stats.samples == 1 && stats.p50 == 10ms && stats.p95 == 10ms && stats.p99 == 10ms);
    check(stats.mean_ms == 10.0 && stats.minimum == 10ms && stats.maximum == 10ms);
    const Duration two[]{20ms, 10ms};
    stats = fluxcrawl::latency_stats(two);
    check(stats.p50 == 10ms && stats.p95 == 20ms && stats.p99 == 20ms);
    check(stats.mean_ms == 15.0);
    const Duration five[]{50ms, 10ms, 30ms, 20ms, 40ms};
    stats = fluxcrawl::latency_stats(five);
    check(stats.p50 == 30ms && stats.p95 == 50ms && stats.p99 == 50ms);
    check(stats.mean_ms == 30.0 && stats.minimum == 10ms && stats.maximum == 50ms);
    const Duration huge[]{Duration::max(), Duration::max()};
    check(fluxcrawl::latency_stats(huge).mean_ms.value() > 0);
    bool rejected = false;
    try { const Duration negative[]{-1ns}; (void)fluxcrawl::latency_stats(negative); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
    std::vector<Duration> large;
    for (int i = 1; i <= 1000; ++i) { large.emplace_back(std::chrono::milliseconds{i}); }
    stats = fluxcrawl::latency_stats(large);
    check(stats.p50 == 500ms && stats.p95 == 950ms && stats.p99 == 990ms);
    fluxcrawl::CrawlResult result;
    result.pages_attempted = 100;
    result.responses_received = 80;
    result.bytes_downloaded = 1024;
    result.elapsed = 2s;
    auto metrics = fluxcrawl::crawl_metrics(result);
    check(metrics.attempts_per_second == 50.0 && metrics.responses_per_second == 40.0
          && metrics.bytes_per_second == 512.0 && !metrics.latency.p99);
    result.elapsed = 0ns;
    check(!fluxcrawl::crawl_metrics(result).attempts_per_second);
    result = {};
    result.elapsed = 1s;
    check(fluxcrawl::crawl_metrics(result).attempts_per_second == 0.0);
    std::cout << failures << " metric failures\n";
    return failures ? 1 : 0;
}
