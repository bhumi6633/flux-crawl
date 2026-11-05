#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <iostream>
#include <map>
#include <stdexcept>
int main(int argc, char** argv)
{
    if (argc != 8) { return 2; }
    try {
        const auto seed = fluxcrawl::Url::parse(argv[1]).value();
        const std::string engine{argv[2]};
        fluxcrawl::CrawlOptions options;
        options.respect_robots = true;
        options.max_depth = 2;
        options.max_pages = std::stoull(argv[3]);
        options.retry.max_retries = 2;
        options.retry.initial_backoff = std::chrono::milliseconds{80};
        options.retry.max_backoff = std::chrono::milliseconds{320};
        options.retry.jitter = false;
        options.scheduling.max_per_host = std::stoull(argv[4]);
        const auto rate = std::stod(argv[5]);
        if (rate != 0) { options.scheduling.rate = fluxcrawl::RateLimit{rate, 1}; }
        const auto concurrency = std::stoull(argv[6]);
        options.profile = std::string{argv[7]} == "1";
        fluxcrawl::HttpClientOptions http;
        http.use_environment_proxy = false;
        http.request_timeout = std::chrono::seconds{5};
        const auto result = [&] {
            if (engine == "sequential") { return fluxcrawl::SequentialCrawler{http}.crawl(seed, options); }
            if (engine == "threaded") { return fluxcrawl::ThreadPoolCrawler{concurrency, http}.crawl(seed, options); }
            return fluxcrawl::AsyncCrawler{concurrency, http}.crawl(seed, options);
        }();
        if (result.pages_attempted > options.max_pages || result.pages_attempted != result.records.size()
            || result.robots_fetches != result.robots_records.size()
            || result.resources_completed + result.urls_blocked_by_robots + result.robots_resources_reused + result.urls_pending != result.urls_discovered
            || result.robots_retries_scheduled != result.robots_retries_attempted + result.robots_retries_cancelled) {
            throw std::runtime_error("Inconsistent policy accounting");
        }
        std::map<std::string, std::size_t> attempts;
        for (const auto& record : result.records) {
            auto& attempt = attempts[record.item.url.str()];
            if (record.item.policy_request || record.item.retry_count != attempt++) { throw std::runtime_error("Duplicate resource work"); }
            std::cout << "resource\t" << record.item.url.str() << '\t' << record.item.depth << '\t' << record.item.retry_count << '\n';
        }
        for (const auto& record : result.robots_records) {
            if (!record.item.policy_request) { throw std::runtime_error("Non-bootstrap policy record"); }
            std::cout << "robots\t" << record.item.url.str() << '\t' << record.item.retry_count << '\t';
            if (const auto* response = std::get_if<fluxcrawl::CrawlResponseMetadata>(&record.outcome)) { std::cout << response->status_code; }
            else { std::cout << "error"; }
            std::cout << '\n';
        }
        std::cout << "summary\t" << result.pages_attempted << '\t' << result.urls_discovered << '\t' << result.urls_pending << '\t'
            << result.robots_fetches << '\t' << result.robots_origins << '\t' << result.robots_cache_hits << '\t'
            << result.robots_allowed << '\t' << result.urls_blocked_by_robots << '\t' << result.robots_fetch_failures << '\t'
            << result.robots_resources_reused << '\t' << result.robots_retries_scheduled << '\t' << result.robots_retries_attempted << '\t'
            << result.robots_retries_cancelled << '\t' << result.retries_attempted << '\t' << result.responses_recovered_after_retry << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
