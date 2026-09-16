#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>

#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

int main(int argc, char* argv[])
{
    if (argc != 3) { return 2; }
    int checks = 0;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    const auto url = [&](const std::string& path) {
        auto parsed = fluxcrawl::Url::parse(std::string{argv[1]} + path);
        if (!parsed) { throw std::runtime_error("Invalid fixture URL"); }
        return *parsed;
    };
    fluxcrawl::HttpClientOptions http;
    http.connect_timeout = std::chrono::milliseconds{200};
    const fluxcrawl::SequentialCrawler sequential{http};
    const auto identities = [&](const fluxcrawl::CrawlResult& result) {
        std::map<std::string, std::size_t> entries;
        for (const auto& record : result.records) {
            check(entries.emplace(record.item.url.canonical(), record.item.depth).second, "no duplicate attempts");
        }
        check(result.pages_attempted == result.records.size(), "record accounting");
        check(result.pages_attempted == result.responses_received + result.fetch_failures, "response accounting");
        check(result.urls_discovered == result.pages_attempted + result.urls_pending, "discovery accounting");
        return entries;
    };
    const auto compare = [&](const std::string& path, fluxcrawl::CrawlOptions options, std::size_t workers) {
        const auto reference = sequential.crawl(url(path), options);
        const auto actual = fluxcrawl::ThreadPoolCrawler{workers, http}.crawl(url(path), options);
        check(identities(reference) == identities(actual), "sequential URL/depth equivalence");
        check(reference.responses_received == actual.responses_received && reference.fetch_failures == actual.fetch_failures
              && reference.html_pages_processed == actual.html_pages_processed
              && reference.urls_discovered == actual.urls_discovered, "sequential counters equivalent");
        std::map<std::string, std::pair<std::string, long>> responses;
        for (const auto& r : reference.records) {
            if (const auto* metadata = std::get_if<fluxcrawl::CrawlResponseMetadata>(&r.outcome)) {
                responses.emplace(r.item.url.canonical(), std::pair{metadata->effective_url.canonical(), metadata->status_code});
            }
        }
        for (const auto& r : actual.records) {
            if (const auto* metadata = std::get_if<fluxcrawl::CrawlResponseMetadata>(&r.outcome)) {
                check(responses.at(r.item.url.canonical()) == std::pair{metadata->effective_url.canonical(), metadata->status_code}, "response metadata equivalent");
            }
        }
    };
    for (const std::size_t workers : {1U, 2U, 4U, 8U, 16U}) {
        compare("/crawl/", {}, workers);
        compare("/crawl/shared", {}, workers); // Empty seed expansion, surplus workers.
    }
    compare("/crawl/", {1, 100, false}, 8);
    compare("/crawl/", {0, 100, false}, 8);
    compare("/crawl/failures", {}, 8);
    compare("/crawl/host-policy", {2, 100, true}, 8);
    compare("/depth-race/root", {3, 100, false}, 4);
    const auto zero = fluxcrawl::ThreadPoolCrawler{16, http}.crawl(url("/crawl/"), {2, 0, false});
    check(zero.pages_attempted == 0 && zero.urls_discovered == 1 && zero.urls_pending == 1, "zero budget terminates");
    bool invalid = false;
    try { const fluxcrawl::ThreadPoolCrawler bad{0}; }
    catch (const std::invalid_argument&) { invalid = true; }
    check(invalid, "zero workers rejected");

    const auto stats = [&](const std::string& run) {
        const auto fetched = fluxcrawl::HttpClient{http}.fetch(url("/pool/" + run + "/stats"));
        const auto* response = std::get_if<fluxcrawl::HttpResponse>(&fetched);
        if (!response) { throw std::runtime_error("No server stats"); }
        std::istringstream stream{response->body};
        std::size_t peak = 0, shared = 0;
        stream >> peak >> shared;
        return std::pair{peak, shared};
    };
    for (const std::size_t workers : {1U, 4U}) {
        const auto run = "overlap" + std::to_string(workers);
        const auto result = fluxcrawl::ThreadPoolCrawler{workers, http}.crawl(url("/pool/" + run + "/root"));
        check(identities(result).size() == 6 && result.fetch_failures == 0, "delayed expansion completes");
        const auto [peak, shared] = stats(run);
        check(workers == 1 ? peak == 1 : peak > 1, "server proves concurrent request overlap");
        check(shared == 1, "server confirms shared fetched once");
        std::cout << workers << " workers: server peak active requests = " << peak << '\n';
    }
    for (int repeat = 0; repeat < 25; ++repeat) {
        const auto run = "race" + std::to_string(repeat);
        const auto result = fluxcrawl::ThreadPoolCrawler{8, http}.crawl(url("/pool/" + run + "/root"));
        check(identities(result).size() == 6 && result.urls_pending == 0, "repeated termination and dedup");
        check(stats(run).second == 1, "repeated server duplicate check");
        const auto limited = fluxcrawl::ThreadPoolCrawler{16, http}.crawl(url("/stress/limit" + std::to_string(repeat) + "/root"), {2, 5, false});
        check(identities(limited).size() == 5 && limited.pages_attempted == 5 && limited.fetch_failures == 0, "page-limit race exact budget");
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto result = fluxcrawl::ThreadPoolCrawler{workers, http}.crawl(url("/stress/full" + std::to_string(workers) + "/root"), {3, 200, false});
        check(identities(result).size() == 102 && result.responses_received == 102 && result.urls_pending == 0, "102-page stress complete");
    }
    std::cout << checks << " threaded checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
