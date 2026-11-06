#pragma once

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace fluxcrawl {
using ProfileDuration = std::chrono::nanoseconds;

struct CommonProfile {
    // Entire extractor: DOM traversal, URL resolution, per-document dedup and cleanup.
    // Bounded mode uses temporary disk dedup; legacy benchmark uses DOM/XPath.
    ProfileDuration extraction{}, discovery{}, frontier{}, retry_delay{};
    std::size_t retry_decisions = 0;
    std::size_t extractions = 0, links_considered = 0, links_accepted = 0;
    void merge(const CommonProfile& other) noexcept
    {
        retry_decisions += other.retry_decisions;
        retry_delay += std::min(other.retry_delay, ProfileDuration::max() - retry_delay);
        extraction += other.extraction; discovery += other.discovery; frontier += other.frontier;
        extractions += other.extractions; links_considered += other.links_considered;
        links_accepted += other.links_accepted;
    }
};
struct WorkerProfile {
    CommonProfile common;
    ProfileDuration lock_wait{}, idle{}, activity{};
    std::size_t lock_acquisitions = 0, waits = 0, wakeups = 0, requests = 0;
    void merge(const WorkerProfile& other) noexcept
    {
        common.merge(other.common);
        lock_wait += other.lock_wait; idle += other.idle; activity += other.activity;
        lock_acquisitions += other.lock_acquisitions; waits += other.waits;
        wakeups += other.wakeups; requests += other.requests;
    }
};
struct ThreadProfile {
    WorkerProfile total;
    std::vector<WorkerProfile> workers;
    std::size_t max_active = 0;
};
struct AsyncProfile {
    ProfileDuration multi_wait{}, completions_time{}, refill_time{};
    std::size_t iterations = 0, waits = 0, completions = 0, refills = 0, max_active = 0;
};
struct ProfileSummary {
    CommonProfile common;
    std::optional<ThreadProfile> threaded;
    std::optional<AsyncProfile> async;
};
} // namespace fluxcrawl
