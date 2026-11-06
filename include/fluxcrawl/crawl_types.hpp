#pragma once

#include <fluxcrawl/crawl_frontier.hpp>
#include <fluxcrawl/host_scheduler.hpp>
#include <fluxcrawl/http_response.hpp>

#include <fluxcrawl/profiling.hpp>
#include <fluxcrawl/backpressure.hpp>
#include <fluxcrawl/checkpoint_control.hpp>
#include <fluxcrawl/persistence_control.hpp>
#include <fluxcrawl/retry_policy.hpp>
#include <vector>
#include <chrono>

namespace fluxcrawl {

struct CrawlOptions {
    std::size_t max_depth = 2;
    std::size_t max_pages = 100;
    bool same_host_only = false;
    bool profile = false;
    SchedulingOptions scheduling{};
    BackpressureOptions backpressure{};
    RetryPolicy retry{};
    bool respect_robots = false;
    CheckpointControl checkpoint{};
    SqliteControl sqlite{};
    std::shared_ptr<const CrawlCheckpoint> resume_from{};
};

struct CrawlResponseMetadata {
    Url effective_url;
    long status_code;
    std::string content_type;
    std::size_t downloaded_bytes;
    std::optional<std::chrono::seconds> retry_after{};
};

struct CrawlRecord {
    CrawlItem item;
    std::variant<CrawlResponseMetadata, HttpError> outcome;
    // Active transfer duration; excludes frontier wait and HTML processing.
    std::chrono::nanoseconds latency{};
};

struct CrawlResult {
    std::size_t robots_fetches = 0, robots_origins = 0, robots_cache_hits = 0;
    std::size_t robots_allowed = 0, urls_blocked_by_robots = 0, robots_fetch_failures = 0;
    std::size_t robots_retries_scheduled = 0, robots_retries_attempted = 0;
    std::size_t robots_resources_reused = 0, robots_retries_cancelled = 0;
    std::size_t recovery_attempts = 0, recovery_retry_attempts = 0;
    std::size_t attempts_unresolved = 0, robots_attempts_unresolved = 0;
    // Ephemeral baselines keep current-session throughput separate from cumulative totals.
    std::size_t session_start_attempts = 0, session_start_responses = 0, session_start_bytes = 0;
    QueuePressureMetrics pressure{};
    PersistenceMetrics persistence{};
    std::vector<std::string> persistence_errors;
    std::chrono::nanoseconds checkpoint_time{};
    std::size_t checkpoints_written = 0, checkpoint_bytes = 0;
    std::vector<std::string> checkpoint_errors; // Session diagnostics, not persisted.
    std::size_t pages_attempted = 0;
    std::size_t responses_received = 0;
    // All HttpError results, including transport, body-limit, and metadata failures.
    std::size_t fetch_failures = 0;
    std::size_t retries_scheduled = 0, retries_attempted = 0, retries_exhausted = 0;
    std::size_t responses_recovered_after_retry = 0, retry_after_used = 0;
    std::size_t retries_cancelled = 0; // Scheduled but unexecuted at stop; resumable from a snapshot.
    std::size_t resources_completed = 0, terminal_http_failures = 0, terminal_fetch_failures = 0;
    std::size_t html_pages_processed = 0;
    std::size_t urls_discovered = 0; // Accepted by CrawlState, including the seed.
    std::size_t urls_pending = 0;
    std::chrono::nanoseconds elapsed{};
    // Sum of decoded body bytes in HttpResponse outcomes; excludes partial failed bodies.
    std::size_t bytes_downloaded = 0;
    std::optional<ProfileSummary> profile{};
    std::vector<CrawlRecord> robots_records; // Infrastructure attempts, no bodies or HTML discovery.
    std::vector<CrawlRecord> records; // Sequential attempt order or concurrent publication order; no bodies.
};

} // namespace fluxcrawl
