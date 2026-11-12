#pragma once

#include <fluxcrawl/crawl_frontier.hpp>
#include <chrono>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace fluxcrawl {
class CrawlMutationSink;

struct HostKey {
    std::string scheme, host;
    std::uint16_t effective_port;
    static HostKey from(const Url& url);
    bool operator==(const HostKey&) const = default;
};
struct HostKeyHash {
    std::size_t operator()(const HostKey& key) const noexcept;
};
struct RateLimit {
    double requests_per_second;
    std::size_t burst = 1;
};
struct SchedulingOptions {
    std::optional<std::size_t> max_per_host{};
    std::optional<RateLimit> rate{};
};

// Explicit timestamps allow deterministic tests; callers supply monotonic time.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    explicit TokenBucket(std::optional<RateLimit> limit = std::nullopt);
    [[nodiscard]] Time eligible_at(Time now);
    bool consume(Time now);
    void start_empty(Time now) { if (limit_) { tokens_ = 0; updated_ = now; } }
private:
    void refill(Time now);
    std::optional<RateLimit> limit_;
    long double tokens_ = 0;
    std::optional<Time> updated_;
};

// Not internally synchronized. Threaded callers serialize ALL operations,
// including Lease destruction/release. The scheduler must outlive every lease.
// Deduplication belongs to CrawlState. Hosts retain tokens until crawl teardown.
class HostScheduler {
    struct Host;
public:
    using Time = TokenBucket::Time;
    using Clock = TokenBucket::Clock;
    struct Work {
        CrawlItem item;
        std::optional<Time> eligible_at{};
        bool was_active = false;
    };
    class Lease {
    public:
        CrawlItem item;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        void release() noexcept;
    private:
        friend class HostScheduler;
        Lease(CrawlItem work, HostScheduler* owner, Host* host, std::uint64_t id);
        HostScheduler* owner_;
        Host* host_;
        std::uint64_t id_;
    };
    explicit HostScheduler(std::size_t global_limit, SchedulingOptions options = {}, bool track_active = false, bool cold_limits = false, bool telemetry = false);
    [[nodiscard]] std::vector<Work> snapshot() const;
    HostScheduler(const HostScheduler&) = delete;
    HostScheduler& operator=(const HostScheduler&) = delete;
    void set_mutation_sink(CrawlMutationSink* sink) noexcept { sink_ = sink; }
    void mark_terminal(const CrawlItem& item);
    void note_activity();
    void enqueue(CrawlItem item);
    void delay(CrawlItem item, Time eligible_at);
    struct HostSample {
        std::string origin;
        std::size_t pending = 0, active = 0, delayed = 0, completed = 0;
        double wait_ms = 0;
    };
    struct Sample {
        std::size_t total_hosts = 0, candidates = 0;
        std::vector<HostSample> hosts;
        std::optional<double> next_retry_ms;
    };
    // Bounded candidate scan, never copies queued URLs. Call under owner lock.
    [[nodiscard]] Sample sample(Time now, std::size_t limit = 32);
    [[nodiscard]] std::size_t delayed() const noexcept { return delayed_.size(); }
    [[nodiscard]] std::optional<Lease> acquire(Time now);
    // Minimum delayed-work deadline and applicable host token deadline.
    // Promotes due retries; may return now if work became eligible since acquire.
    // Capacity/depth blocks requiring completion need no timer.
    [[nodiscard]] std::optional<Time> next_deadline(Time now);
    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }
    [[nodiscard]] std::size_t resource_pending() const noexcept { return pending_ - infrastructure_pending_; }
    [[nodiscard]] std::size_t active() const noexcept { return active_; }
private:
    struct Host {
        explicit Host(std::optional<RateLimit> rate) : bucket{rate} {}
        std::map<std::size_t, std::deque<CrawlItem>> queues;
        TokenBucket bucket;
        std::size_t active = 0, pending = 0, delayed = 0, completed = 0;
    };
    void promote(Time now);
    bool depth_eligible() const noexcept;
    void finish(Host& host, std::uint64_t id) noexcept;
    CrawlMutationSink* sink_ = nullptr;
    bool track_active_, cold_limits_, telemetry_;
    std::uint64_t next_id_ = 0;
    std::map<std::uint64_t, CrawlItem> active_items_;
    SchedulingOptions options_;
    std::size_t infrastructure_pending_ = 0;
    std::size_t global_limit_, pending_ = 0, active_ = 0, active_depth_ = 0;
    std::unordered_map<HostKey, Host, HostKeyHash> hosts_;
    // Hash-node addresses survive rehash; hosts are never erased.
    std::list<Host*> ready_;
    std::map<std::size_t, std::size_t> depths_;
    // Ordered deadlines; equivalent-key insertion order supplies stable ties.
    std::multimap<Time, CrawlItem> delayed_;
};
} // namespace fluxcrawl
