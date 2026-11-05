#include <fluxcrawl/retry_policy.hpp>
#include <fluxcrawl/host_scheduler.hpp>
#include <curl/curl.h>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
using namespace fluxcrawl;
using namespace std::chrono_literals;
void check(bool value) { if (!value) { throw std::runtime_error("Retry policy/scheduler check failed"); } }
int main()
{
    try {
        for (long status : {429, 500, 502, 503, 504}) { check(retryable_status(status)); }
        for (long status : {200, 204, 301, 400, 401, 403, 404, 501}) { check(!retryable_status(status)); }
        for (int code : {CURLE_OPERATION_TIMEDOUT, CURLE_COULDNT_CONNECT, CURLE_COULDNT_RESOLVE_HOST,
                         CURLE_RECV_ERROR, CURLE_SEND_ERROR, CURLE_GOT_NOTHING, CURLE_PARTIAL_FILE}) {
            check(retryable_error({HttpErrorCode::CurlError, code, "original error"}));
            check(!retryable_error({HttpErrorCode::BodyTooLarge, code, "body cap"}));
        }
        for (auto code : {HttpErrorCode::BufferFailure, HttpErrorCode::InvalidEffectiveUrl, HttpErrorCode::InvalidResponse}) {
            check(!retryable_error({code, CURLE_RECV_ERROR, "client failure"}));
        }
        check(!retryable_error({HttpErrorCode::CurlError, CURLE_SSL_CACERT, "TLS"}));
        RetryPolicy policy{5, 100ms, 1000ms, 2, false};
        int expected[] = {100, 200, 400, 800, 1000, 1000};
        for (std::size_t i = 0; i < 6; ++i) { check(policy.delay(i + 1).count() == expected[i]); }
        check(policy.delay(std::numeric_limits<std::size_t>::max()) == 1000ms);
        policy.multiplier = 1; check(policy.delay(std::numeric_limits<std::size_t>::max()) == 100ms);
        policy.multiplier = std::numeric_limits<double>::max(); check(policy.delay(2) == 1000ms);
        policy.multiplier = 2; policy.jitter = true;
        check(policy.delay(3, 0) == 0ms && policy.delay(3, 0.5) == 200ms && policy.delay(3, 1) == 400ms);
        std::mt19937_64 rng{123};
        for (int i = 0; i < 1000; ++i) {
            const auto value = policy.delay(5, std::generate_canonical<double, 53>(rng));
            check(value >= 0ms && value <= 1000ms);
        }
        for (auto value : {"-1", "+2", "1.5", "junk", "2x", "999999999999999999999999", "Wed, 21 Oct 2015 07:28:00 GMT", ""}) {
            check(!parse_retry_after(value));
        }
        check(parse_retry_after(" 2\r\n") == 2s && parse_retry_after("0") == 0s);
        for (double multiplier : {0.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            auto bad = policy; bad.multiplier = multiplier;
            bool rejected = false;
            try { bad.validate(); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected);
        }
        for (auto bad : {RetryPolicy{1, 0ms}, RetryPolicy{1, 100ms, 99ms}}) {
            bool rejected = false;
            try { bad.validate(); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected);
        }
        const HostScheduler::Time now{};
        HostScheduler scheduler{2, {.max_per_host = 1, .rate = RateLimit{2, 1}}};
        auto item = [](const char* path) { return CrawlItem{Url::parse(std::string{"http://a.test/"} + path).value(), 1, 1}; };
        scheduler.delay(item("a"), now + 500ms);
        scheduler.delay(item("b"), now + 100ms);
        scheduler.delay(item("c"), now + 300ms);
        check(scheduler.pending() == 3 && scheduler.delayed() == 3 && !scheduler.acquire(now));
        check(scheduler.next_deadline(now) == now + 100ms);
        auto b = scheduler.acquire(now + 100ms); check(b && b->item.url.path() == "/b");
        check(!scheduler.acquire(now + 500ms)); // active host cap
        b.reset();
        check(!scheduler.acquire(now + 500ms)); // rate is still depleted
        check(scheduler.next_deadline(now + 500ms) == now + 600ms);
        auto c = scheduler.acquire(now + 600ms); check(c && c->item.url.path() == "/c"); c.reset();
        auto a = scheduler.acquire(now + 1100ms); check(a && a->item.url.path() == "/a"); a.reset();
        check(scheduler.pending() == 0 && scheduler.active() == 0);
        HostScheduler ties{1}; ties.delay(item("a"), now); ties.delay(item("b"), now);
        auto first = ties.acquire(now); check(first->item.url.path() == "/a"); first.reset();
        check(ties.acquire(now)->item.url.path() == "/b");
        HostScheduler depths{2}; depths.delay(item("shallow"), now + 1s);
        depths.enqueue({Url::parse("http://b.test/deep").value(), 2});
        check(!depths.acquire(now) && depths.next_deadline(now) == now + 1s);
        auto shallow = depths.acquire(now + 1s); check(shallow->item.depth == 1);
        check(!depths.acquire(now + 1s)); shallow.reset(); check(depths.acquire(now + 1s)->item.depth == 2);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Retry classification, capped backoff, deterministic jitter and delayed scheduling passed\n";
}
