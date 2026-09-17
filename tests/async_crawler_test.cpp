#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>

#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

int main(int argc, char* argv[])
{
    if (argc != 3) { return 2; }
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    const auto url = [&](const std::string& path) { return fluxcrawl::Url::parse(std::string{argv[1]} + path).value(); };
    fluxcrawl::HttpClientOptions http;
    http.connect_timeout = std::chrono::milliseconds{200};
    http.request_timeout = std::chrono::milliseconds{5000};
    const auto identities = [&](const fluxcrawl::CrawlResult& result) {
        std::map<std::string, std::size_t> entries;
        std::size_t bytes = 0;
        for (const auto& record : result.records) {
            check(record.latency.count() >= 0 && record.latency <= result.elapsed, "active latency bounded by crawl elapsed");
            if (const auto* response = std::get_if<fluxcrawl::CrawlResponseMetadata>(&record.outcome)) {
                bytes += response->downloaded_bytes;
            }
            check(entries.emplace(record.item.url.canonical(), record.item.depth).second, "no duplicate attempts");
        }
        check(bytes == result.bytes_downloaded, "decoded body byte total");
        check(result.pages_attempted == result.records.size(), "record accounting");
        check(result.pages_attempted == result.responses_received + result.fetch_failures, "outcome accounting");
        check(result.urls_discovered == result.pages_attempted + result.urls_pending, "discovery accounting");
        return entries;
    };
    const auto equivalent = [&](const fluxcrawl::CrawlResult& reference, const fluxcrawl::CrawlResult& actual) {
        check(identities(reference) == identities(actual), "URL and shortest-depth equivalence");
        check(reference.responses_received == actual.responses_received && reference.fetch_failures == actual.fetch_failures
            && reference.html_pages_processed == actual.html_pages_processed, "counter equivalence");
        std::map<std::string, const fluxcrawl::CrawlRecord*> expected;
        for (const auto& r : reference.records) { expected.emplace(r.item.url.canonical(), &r); }
        for (const auto& r : actual.records) {
            const auto found = expected.find(r.item.url.canonical());
            if (found == expected.end()) { continue; }
            const auto& outcome = found->second->outcome;
            check(outcome.index() == r.outcome.index(), "outcome kind equivalence");
            if (const auto* metadata = std::get_if<fluxcrawl::CrawlResponseMetadata>(&r.outcome)) {
                const auto* old = std::get_if<fluxcrawl::CrawlResponseMetadata>(&outcome);
                check(old && old->effective_url == metadata->effective_url && old->status_code == metadata->status_code
                    && old->content_type == metadata->content_type && old->downloaded_bytes == metadata->downloaded_bytes,
                    "complete response metadata equivalence");
            } else if (const auto* old = std::get_if<fluxcrawl::HttpError>(&outcome)) {
                const auto& error = std::get<fluxcrawl::HttpError>(r.outcome);
                check(old->code == error.code && old->curl_code == error.curl_code, "error classification equivalence");
            }
        }
    };
    const auto compare = [&](const std::string& path, fluxcrawl::CrawlOptions options, std::size_t concurrency) {
        const auto actual = fluxcrawl::AsyncCrawler{concurrency, http}.crawl(url(path), options);
        equivalent(fluxcrawl::SequentialCrawler{http}.crawl(url(path), options), actual);
        equivalent(fluxcrawl::ThreadPoolCrawler{4, http}.crawl(url(path), options), actual);
    };
    for (std::size_t concurrency : {1U, 2U, 4U, 8U, 32U, 64U}) {
        compare("/crawl/", {3, 100, false}, concurrency);
        compare("/crawl/shared", {}, concurrency);
    }
    compare("/crawl/", {0, 100, false}, 8);
    compare("/crawl/", {1, 100, false}, 8);
    compare("/crawl/failures", {}, 8);
    compare("/crawl/host-policy", {2, 100, true}, 8);
    compare("/depth-race/root", {3, 100, false}, 4);
    for (const auto* path : {"/redirect", "/loop", "/forbidden", "/invalid-effective", "/gzip", "/binary",
                             "/empty", "/no-content-type", "/chunked", "/truncated", "/agent", "/server-error", "/http-500", "/http-429"}) {
        compare(path, {0, 100, false}, 4);
    }
    const auto zero = fluxcrawl::AsyncCrawler{32, http}.crawl(url("/never-request-zero"), {2, 0, false});
    check(zero.pages_attempted == 0 && zero.records.empty() && zero.urls_pending == 1, "zero budget");
    bool invalid = false;
    try { const fluxcrawl::AsyncCrawler bad{0}; }
    catch (const std::invalid_argument&) { invalid = true; }
    check(invalid, "zero concurrency rejected");
    auto invalid_http = http;
    invalid_http.request_timeout = std::chrono::milliseconds{0};
    invalid = false;
    try { const fluxcrawl::AsyncCrawler bad{4, invalid_http}; }
    catch (const std::invalid_argument&) { invalid = true; }
    check(invalid, "invalid HTTP options rejected");

    const auto body = [&](const std::string& path) {
        auto fetched = fluxcrawl::HttpClient{http}.fetch(url(path));
        return std::get<fluxcrawl::HttpResponse>(fetched).body;
    };
    for (std::size_t concurrency : {1U, 3U, 4U}) {
        const auto run = "overlap" + std::to_string(concurrency);
        auto result = fluxcrawl::AsyncCrawler{concurrency, http}.crawl(url("/pool/" + run + "/root"));
        check(identities(result).size() == 6 && result.fetch_failures == 0, "overlap graph complete");
        std::istringstream stats{body("/pool/" + run + "/stats")};
        std::size_t peak = 0, shared = 0;
        stats >> peak >> shared;
        check(concurrency == 1 ? peak == 1 : peak > 1, "server proves overlap");
        check(peak <= concurrency && shared == 1, "concurrency cap and server dedup");
        std::cout << "Concurrency " << concurrency << ": server peak = " << peak << '\n';
    }
    const fluxcrawl::AsyncCrawler reusable{8, http};
    for (int repeat = 0; repeat < 10; ++repeat) {
        const auto prefix = "/refill/" + std::to_string(repeat);
        auto result = fluxcrawl::AsyncCrawler{3, http}.crawl(url(prefix + "/root"));
        check(identities(result).size() == 5 && result.fetch_failures == 0, "refill graph complete");
        check(body(prefix + "/result") == "1", "D starts while A and C remain active");
        const auto chain = reusable.crawl(url("/crawl/chain"));
        check(identities(chain).size() == 3 && chain.urls_pending == 0, "temporary empty frontier waits for discoveries");
        const auto limited_prefix = "/stress/limit" + std::to_string(repeat);
        const auto limited = reusable.crawl(url(limited_prefix + "/root"), {3, 5, false});
        check(identities(limited).size() == 5 && limited.pages_attempted == 5, "exact concurrent page budget");
        std::size_t peak = 0, total = 0, most = 0;
        std::istringstream stats{body(limited_prefix + "/stats")};
        stats >> peak >> total >> most;
        check(total == 5 && most == 1 && peak <= 8, "server confirms exactly five requests");
    }
    for (std::size_t concurrency : {1U, 2U, 4U, 8U, 16U}) {
        const auto prefix = "/stress/full" + std::to_string(concurrency);
        const auto result = fluxcrawl::AsyncCrawler{concurrency, http}.crawl(url(prefix + "/root"), {3, 200, false});
        check(identities(result).size() == 102 && result.responses_received == 102 && result.urls_pending == 0,
              "102-page high fanout completes");
        std::size_t peak = 0, total = 0, most = 0;
        std::istringstream stats{body(prefix + "/stats")};
        stats >> peak >> total >> most;
        check(peak <= concurrency && total == 102 && most == 1, "high fanout server cap and dedup");
    }
    auto small = http;
    small.max_body_bytes = 256;
    for (int repeat = 0; repeat < 10; ++repeat) {
        const auto result = fluxcrawl::AsyncCrawler{4, small}.crawl(url("/crawl/isolated"), {1, 100, false});
        check(identities(result).size() == 5 && result.fetch_failures == 1 && result.responses_received == 4,
              "oversized transfer isolated from three successful siblings");
        for (const auto& r : result.records) {
            if (const auto* error = std::get_if<fluxcrawl::HttpError>(&r.outcome)) {
                check(error->code == fluxcrawl::HttpErrorCode::BodyTooLarge, "body-limit classification");
            }
        }
        const auto failed = reusable.crawl(url("/crawl/transport"), {1, 5, false});
        check(identities(failed).size() == 5 && failed.fetch_failures == 1 && failed.responses_received == 4,
              "transport failure isolated and consumes budget");
    }
    auto timeout = http;
    timeout.request_timeout = std::chrono::milliseconds{50};
    equivalent(fluxcrawl::SequentialCrawler{timeout}.crawl(url("/slow")),
               fluxcrawl::AsyncCrawler{4, timeout}.crawl(url("/slow")));
    std::cout << checks << " async checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
