#include <fluxcrawl/host_scheduler.hpp>
#include <fluxcrawl/persistence_events.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace fluxcrawl {
HostKey HostKey::from(const Url& url)
{
    return {url.scheme(), url.host(), url.port().value_or(url.is_https() ? 443 : 80)};
}
std::size_t HostKeyHash::operator()(const HostKey& key) const noexcept
{
    auto hash = std::hash<std::string>{}(key.scheme);
    const auto mix = [&](std::size_t value) { hash ^= value + 0x9e3779b9U + (hash << 6U) + (hash >> 2U); };
    mix(std::hash<std::string>{}(key.host));
    mix(std::hash<std::uint16_t>{}(key.effective_port));
    return hash;
}
TokenBucket::TokenBucket(std::optional<RateLimit> limit) : limit_{limit}
{
    if (limit_) {
        if (!std::isfinite(limit_->requests_per_second) || limit_->requests_per_second <= 0 || limit_->burst == 0) {
            throw std::invalid_argument("Rate must be finite and positive; burst must be positive");
        }
        tokens_ = static_cast<long double>(limit_->burst);
    }
}
void TokenBucket::refill(Time now)
{
    if (!limit_) { return; }
    if (updated_ && now > *updated_) {
        const auto elapsed = std::chrono::duration<long double>(now - *updated_).count();
        tokens_ = std::min(static_cast<long double>(limit_->burst),
                           tokens_ + elapsed * limit_->requests_per_second);
    }
    if (!updated_ || now > *updated_) { updated_ = now; }
}
TokenBucket::Time TokenBucket::eligible_at(Time now)
{
    refill(now);
    if (!limit_ || tokens_ >= 1) { return now; }
    const std::chrono::duration<long double> delay{(1 - tokens_) / limit_->requests_per_second};
    // Saturate extreme valid rates instead of overflowing a duration conversion.
    const auto room = Time::max() - now;
    if (delay >= std::chrono::duration<long double>(room)) { return Time::max(); }
    return now + std::max(Clock::duration{1}, std::chrono::ceil<Clock::duration>(delay));
}
bool TokenBucket::consume(Time now)
{
    refill(now);
    if (limit_ && tokens_ < 1) { return false; }
    if (limit_) { tokens_ -= 1; }
    return true;
}
HostScheduler::HostScheduler(std::size_t global_limit, SchedulingOptions options, bool track_active, bool cold_limits, bool telemetry)
    : track_active_{track_active}, cold_limits_{cold_limits}, telemetry_{telemetry}, options_{options}, global_limit_{global_limit}
{
    if (global_limit == 0 || (options.max_per_host && *options.max_per_host == 0)) {
        throw std::invalid_argument("Global and per-host concurrency limits must be positive");
    }
    (void)TokenBucket{options.rate}; // Validate even if no work is attempted.
}
void HostScheduler::mark_terminal(const CrawlItem& item) {
    if (telemetry_) {
        const auto found = hosts_.find(HostKey::from(item.url));
        if (found != hosts_.end()) { ++found->second.completed; }
    }
    if (sink_) { sink_->completed(item); }
}
void HostScheduler::note_activity() { if (sink_) { sink_->activity(); } }
void HostScheduler::enqueue(CrawlItem item)
{
    if (sink_) { sink_->work(item, {}, false); }
    auto [entry, created] = hosts_.try_emplace(HostKey::from(item.url), options_.rate);
    auto& host = entry->second;
    if (created && cold_limits_) { host.bucket.start_empty(Clock::now()); }
    const auto depth = item.depth;
    const bool infrastructure = item.policy_request;
    auto [count, unused] = depths_.try_emplace(depth, 0);
    (void)unused;
    const bool new_ready = host.queues.empty();
    bool listed = false;
    try {
        if (new_ready) { ready_.push_back(&host); listed = true; }
        host.queues[depth].push_back(std::move(item));
    } catch (...) {
        if (listed) { ready_.pop_back(); }
        const auto queue = host.queues.find(depth);
        if (queue != host.queues.end() && queue->second.empty()) { host.queues.erase(queue); }
        if (count->second == 0) { depths_.erase(count); }
        throw;
    }
    ++host.pending;
    ++count->second;
    ++pending_;
    if (infrastructure) { ++infrastructure_pending_; }
}
void HostScheduler::delay(CrawlItem item, Time eligible_at)
{
    if (sink_) { sink_->work(item, eligible_at, false); }
    const auto depth = item.depth;
    const bool infrastructure = item.policy_request;
    auto [count, inserted] = depths_.try_emplace(depth, 0);
    auto [host, created] = hosts_.try_emplace(HostKey::from(item.url), options_.rate);
    if (created && cold_limits_) { host->second.bucket.start_empty(Clock::now()); }
    try { delayed_.emplace(eligible_at, std::move(item)); }
    catch (...) { if (inserted) { depths_.erase(count); } throw; }
    ++host->second.delayed;
    ++count->second;
    ++pending_;
    if (infrastructure) { ++infrastructure_pending_; }
}
void HostScheduler::promote(Time now)
{
    while (!delayed_.empty() && delayed_.begin()->first <= now) {
        auto entry = delayed_.begin();
        const auto depth = entry->second.depth;
        // Copy first: allocation failure preserves the delayed entry.
        enqueue(entry->second);
        --hosts_.at(HostKey::from(entry->second.url)).delayed;
        --pending_;
        if (entry->second.policy_request) { --infrastructure_pending_; }
        --depths_.at(depth);
        delayed_.erase(entry);
    }
}
bool HostScheduler::depth_eligible() const noexcept
{
    return pending_ != 0 && active_ < global_limit_
        && (active_ == 0 || depths_.begin()->first == active_depth_);
}
std::optional<HostScheduler::Lease> HostScheduler::acquire(Time now)
{
    promote(now);
    if (!depth_eligible()) { return std::nullopt; }
    const auto depth = depths_.begin()->first;
    const auto candidates = ready_.size();
    for (std::size_t index = 0; index < candidates; ++index) {
        auto* host = ready_.front();
        // Rotate without allocation, including temporarily blocked hosts.
        ready_.splice(ready_.end(), ready_, ready_.begin());
        auto queue = host->queues.begin();
        if (queue->first != depth || host->active >= options_.max_per_host.value_or(global_limit_)
            || !host->bucket.consume(now)) { continue; }
        const auto id = ++next_id_;
        if (track_active_) { active_items_.emplace(id, queue->second.front()); }
        Lease lease{std::move(queue->second.front()), this, host, id};
        queue->second.pop_front();
        if (queue->second.empty()) { host->queues.erase(queue); }
        if (host->queues.empty()) { ready_.pop_back(); }
        if (--depths_.begin()->second == 0) { depths_.erase(depths_.begin()); }
        --pending_;
        --host->pending;
        if (lease.item.policy_request) { --infrastructure_pending_; }
        ++active_;
        ++host->active;
        active_depth_ = depth;
        if (sink_) { sink_->work(lease.item, {}, true); sink_->activity(); }
        return lease;
    }
    return std::nullopt;
}
std::optional<HostScheduler::Time> HostScheduler::next_deadline(Time now)
{
    promote(now);
    if (!depth_eligible()) { return std::nullopt; }
    std::optional<Time> deadline;
    if (!delayed_.empty()) { deadline = delayed_.begin()->first; }
    for (auto* host : ready_) {
        if (host->queues.begin()->first != depths_.begin()->first
            || host->active >= options_.max_per_host.value_or(global_limit_)) { continue; }
        const auto time = host->bucket.eligible_at(now);
        if (!deadline || time < *deadline) { deadline = time; }
    }
    return deadline;
}
std::vector<HostScheduler::Work> HostScheduler::snapshot() const
{
    if (!track_active_ && active_ != 0) { throw std::logic_error("Active tracking is required for a recoverable snapshot"); }
    std::vector<Work> work;
    for (auto* host : ready_) {
        for (const auto& [depth, queue] : host->queues) {
            (void)depth;
            for (const auto& item : queue) { work.push_back({item}); }
        }
    }
    for (const auto& [time, item] : delayed_) { work.push_back({item, time}); }
    for (const auto& [id, item] : active_items_) { (void)id; work.push_back({item, std::nullopt, true}); }
    return work;
}
void HostScheduler::finish(Host& host, std::uint64_t id) noexcept
{
    assert(host.active && active_);
    active_items_.erase(id);
    --host.active;
    --active_;
}
HostScheduler::Lease::Lease(CrawlItem work, HostScheduler* owner, Host* host, std::uint64_t id)
    : item{std::move(work)}, owner_{owner}, host_{host}, id_{id} {}
HostScheduler::Lease::Lease(Lease&& other) noexcept
    : item{std::move(other.item)}, owner_{std::exchange(other.owner_, nullptr)}, host_{other.host_}, id_{other.id_} {}
HostScheduler::Lease::~Lease() { release(); }
void HostScheduler::Lease::release() noexcept
{
    if (owner_) { std::exchange(owner_, nullptr)->finish(*host_, id_); }
}
} // namespace fluxcrawl

namespace fluxcrawl {
HostScheduler::Sample HostScheduler::sample(Time now, std::size_t limit)
{
    Sample out; out.total_hosts = hosts_.size();
    if (!delayed_.empty()) { out.next_retry_ms = std::max(0.0, std::chrono::duration<double, std::milli>(delayed_.begin()->first-now).count()); }
    for (auto& [key, host] : hosts_) {
        if (out.candidates++ == 256) { --out.candidates; break; }
        const auto origin = key.scheme + "://" + (key.host.find(':') != std::string::npos ? "[" + key.host + "]" : key.host) + ":" + std::to_string(key.effective_port);
        out.hosts.push_back({origin, host.pending + host.delayed, host.active, host.delayed, host.completed,
            std::max(0.0, std::chrono::duration<double, std::milli>(host.bucket.eligible_at(now)-now).count())});
    }
    std::sort(out.hosts.begin(), out.hosts.end(), [](const auto& a, const auto& b) {
        const auto aw = a.pending+a.active, bw = b.pending+b.active;
        return aw != bw ? aw > bw : a.origin < b.origin;
    });
    out.hosts.resize(std::min(limit, out.hosts.size()));
    return out;
}
}
