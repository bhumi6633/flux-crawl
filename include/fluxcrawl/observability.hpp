#pragma once
#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/metrics.hpp>
#include <atomic>
#include <deque>
#include <memory>
#include <set>

namespace fluxcrawl {
class CrawlState;
class CrawlPolicy;
struct ObservabilitySnapshot {
    unsigned schema_version = 1;
    std::uint64_t sequence = 0;
    std::string engine, state = "connecting";
    std::size_t concurrency = 1, active = 0, max_active = 0, delayed = 0, held = 0;
    std::size_t worker_waits = 0, iterations = 0, completions = 0, successes = 0, waiting_hosts = 0;
    double lock_wait_ms = 0, loop_wait_ms = 0;
    double elapsed_ms = 0, rolling_rps = 0, average_rps = 0;
    CrawlResult counters; // Counter-only copy: never carries request history/bodies.
    LatencyStats latency;
    HostScheduler::Sample hosts;
    BackpressureOptions pressure_options;
    DedupOptions dedup_options;
    SchedulingOptions scheduling;
    std::string persistence_mode;
    std::size_t pending_mutations = 0;
    double last_commit_ms = 0, last_checkpoint_ms = 0;
    bool topology_enabled = false;
    std::vector<std::string> topology_hosts;
    std::vector<std::pair<std::size_t, std::size_t>> topology_edges;
    std::vector<std::string> errors;
};
// Mutations run on the engine owner thread / under its existing coordinator lock.
// Readers only load an immutable, atomically replaced snapshot.
class Observability {
public:
    using Clock = std::chrono::steady_clock;
    explicit Observability(std::chrono::milliseconds interval = std::chrono::milliseconds{500}, bool topology = false);
    void begin(std::string engine, std::size_t concurrency);
    void request_finished(const CrawlRecord& record) noexcept;
    void discovery(const Url& parent, const Url& child) noexcept;
    void worker_wait() { ++worker_waits_; }
    void lock_wait(std::chrono::nanoseconds elapsed) { lock_wait_ += elapsed; }
    void loop_iteration() { ++iterations_; }
    void loop_wait(std::chrono::nanoseconds elapsed) { loop_wait_ += elapsed; }
    void capture(const CrawlState& state, HostScheduler& scheduler, const CrawlPolicy& policy,
                 const CrawlResult& result, const CrawlOptions& options, bool final = false,
                 std::size_t pending_mutations = 0) noexcept;
    void error(std::string message) noexcept;
    // Called after engine teardown on exceptional exit. Preserves last safe counters.
    void failed();
    [[nodiscard]] std::shared_ptr<const ObservabilitySnapshot> latest() const { 
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        return latest_.load();
#else
        return std::atomic_load(&latest_);
#endif
     }
    [[nodiscard]] std::chrono::milliseconds interval() const { return interval_; }
private:
    std::chrono::milliseconds interval_;
    Clock::time_point started_{}, next_{};
    std::string engine_;
    std::size_t concurrency_ = 1, max_active_ = 0, worker_waits_ = 0, iterations_ = 0, completions_ = 0;
    std::size_t last_exhausted_ = 0, successes_ = 0, transfers_finished_ = 0;
    std::chrono::nanoseconds lock_wait_{}, loop_wait_{};
    std::uint64_t sequence_ = 0;
    struct Completion { Clock::time_point time; std::chrono::nanoseconds latency; };
    std::deque<Completion> recent_; // 5s, at most 4096 samples.
    std::deque<std::pair<Clock::time_point, std::size_t>> throughput_; // 100ms buckets, at most 51.
    bool topology_;
    std::vector<std::string> nodes_;
    std::set<std::pair<std::size_t, std::size_t>> edges_;
    std::deque<std::string> errors_;
    void publish(std::shared_ptr<const ObservabilitySnapshot> value) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        latest_.store(std::move(value));
#else
        std::atomic_store(&latest_,std::move(value));
#endif
    }
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const ObservabilitySnapshot>> latest_;
#else
    std::shared_ptr<const ObservabilitySnapshot> latest_;
#endif
};
}
