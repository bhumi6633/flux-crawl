#pragma once

#include <fluxcrawl/crawl_frontier.hpp>
#include <chrono>
#include <list>
#include <map>
#include <unordered_map>

namespace fluxcrawl {

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
    std::optional<std::size_t> max_per_host;
    std::optional<RateLimit> rate;
};

// Explicit timestamps allow deterministic tests; callers supply monotonic time.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    explicit TokenBucket(std::optional<RateLimit> limit = std::nullopt);
    [[nodiscard]] Time eligible_at(Time now);
    bool consume(Time now);
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
        Lease(CrawlItem work, HostScheduler* owner, Host* host);
        HostScheduler* owner_;
        Host* host_;
    };
    explicit HostScheduler(std::size_t global_limit, SchedulingOptions options = {});
    HostScheduler(const HostScheduler&) = delete;
    HostScheduler& operator=(const HostScheduler&) = delete;
    void enqueue(CrawlItem item);
    void delay(CrawlItem item, Time eligible_at);
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
        std::size_t active = 0;
    };
    void promote(Time now);
    bool depth_eligible() const noexcept;
    void finish(Host& host) noexcept;
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
