#include <fluxcrawl/host_scheduler.hpp>
#include <fluxcrawl/crawl_state.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace fluxcrawl;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
Url url(const std::string& value) { return Url::parse(value).value(); }
void add(HostScheduler& scheduler, const std::string& value, std::size_t depth = 1)
{ scheduler.enqueue({url(value), depth}); }
template<class F> void invalid(F function)
{
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid configuration accepted");
}
void keys()
{
    const auto a = HostKey::from(url("HTTPS://Example.com:443/a"));
    const auto b = HostKey::from(url("https://example.com/b"));
    require(a == b && HostKeyHash{}(a) == HostKeyHash{}(b), "normalized default port identity");
    require(a != HostKey::from(url("https://example.com:8443/")), "nondefault port");
    require(a != HostKey::from(url("http://example.com:443/")), "scheme identity");
    require(HostKey::from(url("http://[::1]/")).effective_port == 80, "IPv6 normalized components");
}
void buckets()
{
    const TokenBucket::Time now{};
    TokenBucket disabled;
    for (int i = 0; i < 100; ++i) { require(disabled.consume(now), "disabled unlimited"); }
    TokenBucket bucket{RateLimit{2, 2}};
    require(bucket.consume(now) && bucket.consume(now) && !bucket.consume(now), "initial burst");
    require(bucket.eligible_at(now) == now + 500ms, "next eligibility");
    require(!bucket.consume(now + 250ms), "partial token insufficient");
    require(bucket.consume(now + 500ms) && !bucket.consume(now + 500ms), "refill one token");
    require(bucket.consume(now + 10s) && bucket.consume(now + 10s) && !bucket.consume(now + 10s), "burst cap");
    TokenBucket tiny{RateLimit{std::numeric_limits<double>::min(), 1}};
    require(tiny.consume(now) && tiny.eligible_at(now) == TokenBucket::Time::max(), "tiny rate saturates deadline");
    TokenBucket fast{RateLimit{std::numeric_limits<double>::max(), 1}};
    require(fast.consume(now) && fast.eligible_at(now) > now, "positive sub-tick deadline");
    for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::nan("")}) {
        invalid([&] { (void)TokenBucket{RateLimit{rate, 1}}; });
    }
    invalid([] { (void)TokenBucket{RateLimit{1, 0}}; });
    invalid([] { (void)HostScheduler{0}; });
    invalid([] { (void)HostScheduler{2, {.max_per_host = 0}}; });
}
void scheduling()
{
    const HostScheduler::Time now{};
    HostScheduler scheduler{3, {.max_per_host = 1}};
    require(!scheduler.acquire(now) && !scheduler.next_deadline(now), "empty");
    for (int i = 0; i < 1000; ++i) { add(scheduler, "http://a.test/" + std::to_string(i)); }
    add(scheduler, "http://b.test/1"); add(scheduler, "http://c.test/1");
    auto a = scheduler.acquire(now); auto b = scheduler.acquire(now); auto c = scheduler.acquire(now);
    require(a->item.url.host() == "a.test" && b->item.url.host() == "b.test" && c->item.url.host() == "c.test", "round robin prevents starvation");
    require(scheduler.active() == 3 && !scheduler.acquire(now), "global cap");
    b.reset(); c.reset();
    require(!scheduler.acquire(now) && !scheduler.next_deadline(now), "host cap requires completion");
    a.reset();
    require(scheduler.acquire(now)->item.url.path() == "/1", "completion releases host and preserves FIFO");
    require(scheduler.active() == 0, "temporary lease release");
    // Duplicate detection is deliberately external: scheduler stores both items.
    HostScheduler duplicate{2}; add(duplicate, "http://a.test/x"); add(duplicate, "http://a.test/x");
    require(duplicate.pending() == 2, "scheduler not identity authority");
    CrawlState state{url("http://a.test/x"), 2};
    HostScheduler accepted{2}; state.schedule_pending(accepted);
    require(!state.discover(url("http://A.test:80/x#fragment"), 1), "global seen survives transfer");
    auto claimed = accepted.acquire(now);
    require(!state.discover(claimed->item.url, 1), "active globally seen");
    auto moved = std::move(*claimed); claimed.reset();
    require(accepted.active() == 1, "move transfers ownership");
    moved.release(); moved.release();
    require(accepted.active() == 0, "release idempotent");
}
void depth_and_rates()
{
    const HostScheduler::Time now{};
    HostScheduler scheduler{8, {.max_per_host = 2, .rate = RateLimit{2, 1}}};
    add(scheduler, "http://a.test/1"); add(scheduler, "http://a.test/2");
    auto first = scheduler.acquire(now);
    require(!scheduler.acquire(now) && scheduler.next_deadline(now) == now + 500ms, "pending limited");
    add(scheduler, "http://b.test/1");
    require(scheduler.acquire(now)->item.url.host() == "b.test", "limited host does not block other host");
    first.reset();
    require(scheduler.active() == 0 && scheduler.pending() == 1 && !scheduler.acquire(now), "pending and no active is not done");
    require(scheduler.next_deadline(now + 500ms) == now + 500ms, "elapsed deadline cannot lose wakeup");
    auto second = scheduler.acquire(now + 500ms); second.reset();
    add(scheduler, "http://a.test/3");
    require(!scheduler.acquire(now + 500ms), "empty host retains depleted tokens");
    require(scheduler.acquire(now + 1s).has_value(), "rediscovered host eventually eligible");
    HostScheduler depths{4};
    add(depths, "http://a.test/child", 1); add(depths, "http://b.test/grandchild", 2);
    auto child = depths.acquire(now);
    require(!depths.acquire(now) && !depths.next_deadline(now), "depth barrier");
    add(depths, "http://c.test/sibling", 1);
    require(depths.acquire(now)->item.depth == 1, "same depth proceeds across hosts");
    child.reset();
    require(depths.acquire(now)->item.depth == 2, "depth advances after completion");
}
}
int main()
{
    try { keys(); buckets(); scheduling(); depth_and_rates(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Host identity, token bucket, fairness, depth and lease checks passed\n";
}
