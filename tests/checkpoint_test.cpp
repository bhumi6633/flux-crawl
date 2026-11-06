#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/metrics.hpp>
#include "checkpoint_runtime.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace fluxcrawl;
using namespace std::chrono_literals;
void check(bool v, const char* message) { if (!v) { throw std::runtime_error(message); } }
template<class F> void rejects(F f) { bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } check(rejected, "invalid checkpoint accepted"); }
Url url(const char* value) { return Url::parse(value).value(); }
CrawlCheckpoint sample()
{
    const auto seed = url("http://a.test/");
    CrawlCheckpoint c{.seed = seed};
    c.options.max_depth = 4; c.options.max_pages = 10;
    c.options.retry = {2, 100ms, 1000ms, 2, false};
    c.options.scheduling.max_per_host = 2; c.options.scheduling.rate = RateLimit{5, 3};
    c.options.respect_robots = true;
    c.http.user_agent = "FluxCrawl/1.0 (checkpoint \"test\" \\ path)";
    const auto pending = url("http://b.test/path?q=1");
    const auto retry = url("https://c.test:8443/retry");
    const auto active = url("http://a.test/active");
    c.seen = {retry, active, seed, pending};
    c.unfinished = {{{pending, 2}, {}, false}, {{retry, 3, 2}, 2000, false}, {{active, 1}, {}, true}};
    auto& r = c.counters;
    r.pages_attempted = 4; r.responses_received = 3; r.attempts_unresolved = 1;
    r.resources_completed = 1; r.urls_discovered = 4; r.urls_pending = 3;
    r.retries_scheduled = 2; r.retries_attempted = 1; r.retries_cancelled = 1;
    return c;
}
int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    try {
        auto c = sample();
        auto text = CheckpointManager::serialize(c);
        auto loaded = CheckpointManager::deserialize(text);
        check(CheckpointManager::serialize(loaded) == text, "deterministic serialization round trip");
        check(loaded.unfinished[1].item.depth == 3 && loaded.unfinished[1].item.retry_count == 2, "depth/retry preservation");
        check(loaded.http.user_agent == c.http.user_agent, "quoted escaping round trip");
        check(loaded.unfinished[2].was_active && loaded.counters.pages_attempted == 4, "active recovery and budget persisted");
        rejects([&] { auto bad = c; bad.format_version = 4; (void)CheckpointManager::serialize(bad); });
        for (auto mutation : {std::string{"FLUXCRAWL_CHECKPOINT 4\n"}, text.substr(0, text.size() / 2), text + "junk", std::string{"nonsense"}}) {
            rejects([&] { (void)CheckpointManager::deserialize(mutation); });
        }
        auto replace = [&](const std::string& from, const std::string& to) {
            auto invalid = text; const auto pos = invalid.find(from); check(pos != std::string::npos, "test mutation field exists"); invalid.replace(pos, from.size(), to);
            rejects([&] { (void)CheckpointManager::deserialize(invalid); });
        };
        replace("pages_attempted 4", "pages_attempted -1");
        replace("pages_attempted 4", "pages_attempted 11");
        replace("urls_pending 3", "urls_pending 1");
        replace("http://b.test/path?q=1", "not-a-url");
        replace("sequence 0", "sequence 18446744073709551616");
        replace("retry 2 100", "retry 2 0");
        replace("completed 0", "completed 2");
        rejects([&] { auto bad = c; bad.unfinished.push_back(bad.unfinished[0]); (void)CheckpointManager::serialize(bad); });
        rejects([&] { auto bad = c; bad.unfinished[0].item.depth = 5; (void)CheckpointManager::serialize(bad); });
        rejects([&] { auto bad = c; bad.unfinished[1].item.retry_count = 3; (void)CheckpointManager::serialize(bad); });
        rejects([&] { auto bad = c; bad.unfinished[0].item.url = url("http://unknown.test/"); (void)CheckpointManager::serialize(bad); });
        // Atomic replacement must preserve previous primary on an injected failure.
        const std::filesystem::path file{argv[1]};
        CheckpointManager::write(file, c);
        auto newer = c; newer.sequence = 1;
        rejects([&] { CheckpointManager::write(file, newer, [] { throw std::runtime_error("simulated crash before replacement"); }); });
        check(CheckpointManager::serialize(CheckpointManager::read(file)) == text, "last good checkpoint preserved");
        CheckpointManager::write(file, newer); check(CheckpointManager::read(file).sequence == 1, "atomic replacement succeeds");
        rejects([&] { CheckpointManager::write(file / "missing" / "bad.chk", c); });
        const auto now = HostScheduler::Time{};
        check(CheckpointManager::wall_deadline(now + 1500ms, now, 1000) == 2500, "steady to wall conversion");
        check(CheckpointManager::steady_deadline(2500, now, 1000) == now + 1500ms, "future deadline remains delayed");
        check(CheckpointManager::steady_deadline(2500, now, 3000) == now, "downtime expiration clamps to zero");
        check(CheckpointManager::steady_deadline(std::numeric_limits<std::int64_t>::max(), now, 0) == HostScheduler::Time::max(), "huge deadline saturates");
        TokenBucket cold{RateLimit{2, 3}}; cold.start_empty(now);
        check(!cold.consume(now) && cold.eligible_at(now) == now + 500ms && cold.consume(now + 500ms), "conservative limiter restart");
        // Capture an item whose lease payload was moved into active processing.
        CrawlOptions options; options.max_pages = 2;
        options.checkpoint.on_snapshot = [](const CrawlCheckpoint&) {};
        options.checkpoint.every = 1;
        const auto seed = url("http://a.test/"); const HttpClientOptions http;
        CrawlState state{seed, 2}; HostScheduler scheduler{2, {}, true}; CrawlPolicy policy{false}; CrawlResult result;
        detail::restore_session(seed, options, http, state, scheduler, policy, result);
        auto lease = scheduler.acquire(now); detail::reserve_attempt(result, lease->item);
        auto moved = std::move(lease->item); (void)moved;
        detail::CheckpointRuntime runtime{seed, options, http};
        auto active = runtime.capture(state, scheduler, policy, result);
        check(active && active->unfinished.size() == 1 && active->unfinished[0].was_active, "in-flight item retained after move");
        CheckpointManager::validate(*active);
        auto resume = options; resume.resume_from = std::make_shared<CrawlCheckpoint>(*active);
        CrawlState restored{seed, 2}; HostScheduler rebuilt{8, {}, true, true}; CrawlPolicy fresh{false}; CrawlResult cumulative;
        detail::restore_session(seed, resume, http, restored, rebuilt, fresh, cumulative);
        check(rebuilt.active() == 0 && restored.has_seen(seed), "active counts reset and seen restored");
        check(!restored.discover(seed, 0), "dedup survives resume");
        auto recovered = rebuilt.acquire(now); check(recovered->item.recovery, "active resource requeued");
        detail::reserve_attempt(cumulative, recovered->item);
        check(cumulative.pages_attempted == 2 && cumulative.recovery_attempts == 1 && cumulative.attempts_unresolved == 1, "reissue consumes another attempt");
        cumulative.elapsed = 1s;
        const auto metrics = crawl_metrics(cumulative);
        check(metrics.attempts_per_second == 1, "resume throughput uses current-session attempts only");
        rejects([&] { auto bad = resume; bad.max_depth = 1; detail::restore_session(seed, bad, http, restored, rebuilt, fresh, cumulative); });
        // Reacquired policy for an origin must precede its shallowest work,
        // even if a deeper ready item precedes an active item in serialized order.
        auto mixed = sample();
        mixed.seen[1] = url("http://b.test/active");
        mixed.unfinished[2].item.url = mixed.seen[1];
        auto robots_resume = mixed.options;
        robots_resume.resume_from = std::make_shared<CrawlCheckpoint>(mixed);
        CrawlState robots_state{mixed.seed, mixed.options.max_depth};
        HostScheduler robots_scheduler{8, mixed.options.scheduling, true};
        CrawlPolicy robots_policy{true}; CrawlResult robots_result;
        detail::restore_session(mixed.seed, robots_resume, mixed.http, robots_state, robots_scheduler, robots_policy, robots_result);
        auto first_policy = robots_scheduler.acquire(HostScheduler::Clock::now());
        check(first_policy && first_policy->item.depth == 1 && first_policy->item.url.host() == "b.test", "policy bootstrap preserves shallow depth barrier");
        std::filesystem::remove(file);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Checkpoint round trip, corruption, atomic replacement, clocks, active recovery and dedup passed\n";
}
