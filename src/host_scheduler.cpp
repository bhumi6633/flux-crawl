#include <fluxcrawl/host_scheduler.hpp>
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
HostScheduler::HostScheduler(std::size_t global_limit, SchedulingOptions options)
    : options_{options}, global_limit_{global_limit}
{
    if (global_limit == 0 || (options.max_per_host && *options.max_per_host == 0)) {
        throw std::invalid_argument("Global and per-host concurrency limits must be positive");
    }
    (void)TokenBucket{options.rate}; // Validate even if no work is attempted.
}
void HostScheduler::enqueue(CrawlItem item)
{
    auto& host = hosts_.try_emplace(HostKey::from(item.url), options_.rate).first->second;
    const auto depth = item.depth;
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
    ++count->second;
    ++pending_;
}
bool HostScheduler::depth_eligible() const noexcept
{
    return pending_ != 0 && active_ < global_limit_
        && (active_ == 0 || depths_.begin()->first == active_depth_);
}
std::optional<HostScheduler::Lease> HostScheduler::acquire(Time now)
{
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
        Lease lease{std::move(queue->second.front()), this, host};
        queue->second.pop_front();
        if (queue->second.empty()) { host->queues.erase(queue); }
        if (host->queues.empty()) { ready_.pop_back(); }
        if (--depths_.begin()->second == 0) { depths_.erase(depths_.begin()); }
        --pending_;
        ++active_;
        ++host->active;
        active_depth_ = depth;
        return lease;
    }
    return std::nullopt;
}
std::optional<HostScheduler::Time> HostScheduler::next_deadline(Time now)
{
    if (!depth_eligible()) { return std::nullopt; }
    std::optional<Time> deadline;
    for (auto* host : ready_) {
        if (host->queues.begin()->first != depths_.begin()->first
            || host->active >= options_.max_per_host.value_or(global_limit_)) { continue; }
        const auto time = host->bucket.eligible_at(now);
        if (!deadline || time < *deadline) { deadline = time; }
    }
    return deadline;
}
void HostScheduler::finish(Host& host) noexcept
{
    assert(host.active && active_);
    --host.active;
    --active_;
}
HostScheduler::Lease::Lease(CrawlItem work, HostScheduler* owner, Host* host)
    : item{std::move(work)}, owner_{owner}, host_{host} {}
HostScheduler::Lease::Lease(Lease&& other) noexcept
    : item{std::move(other.item)}, owner_{std::exchange(other.owner_, nullptr)}, host_{other.host_} {}
HostScheduler::Lease::~Lease() { release(); }
void HostScheduler::Lease::release() noexcept
{
    if (owner_) { std::exchange(owner_, nullptr)->finish(*host_); }
}
} // namespace fluxcrawl
