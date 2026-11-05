#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <iostream>
#include <map>
#include <stdexcept>

int main(int argc, char** argv)
{
    if (argc != 10) { return 2; }
    try {
        const auto seed = fluxcrawl::Url::parse(argv[1]).value();
        const std::string engine{argv[2]};
        fluxcrawl::CrawlOptions options;
        options.max_depth = 3;
        options.max_pages = std::stoull(argv[3]);
        options.retry.max_retries = std::stoull(argv[4]);
        options.retry.initial_backoff = std::chrono::milliseconds{std::stoll(argv[5])};
        options.retry.max_backoff = options.retry.initial_backoff * 4;
        options.retry.jitter = std::string{argv[8]} == "1";
        options.scheduling.max_per_host = std::stoull(argv[6]);
        const auto rate = std::stod(argv[7]);
        if (rate != 0) { options.scheduling.rate = fluxcrawl::RateLimit{rate, 1}; }
        const auto concurrency = std::stoull(argv[9]);
        options.profile = true;
        fluxcrawl::HttpClientOptions http;
        http.max_body_bytes = 65536;
        http.use_environment_proxy = false;
        http.request_timeout = std::chrono::seconds{5};
        const auto result = [&] {
            if (engine == "sequential") { return fluxcrawl::SequentialCrawler{http}.crawl(seed, options); }
            if (engine == "threaded") { return fluxcrawl::ThreadPoolCrawler{concurrency, http}.crawl(seed, options); }
            if (engine == "async") { return fluxcrawl::AsyncCrawler{concurrency, http}.crawl(seed, options); }
            throw std::invalid_argument("Unknown engine");
        }();
        if (result.pages_attempted > options.max_pages || result.pages_attempted != result.records.size()
            || result.pages_attempted != result.responses_received + result.fetch_failures
            || result.retries_scheduled != result.retries_attempted + result.retries_cancelled
            || result.resources_completed + result.urls_pending != result.urls_discovered
            || result.profile->common.retry_decisions != result.pages_attempted) {
            throw std::runtime_error("Inconsistent retry accounting");
        }
        std::map<std::string, std::size_t> attempts;
        for (const auto& record : result.records) {
            auto& count = attempts[record.item.url.str()];
            if (record.item.retry_count != count++ || record.item.retry_count > options.retry.max_retries) {
                throw std::runtime_error("Duplicate initial attempt or nonsequential retries");
            }
            std::cout << record.item.url.str() << '\t' << record.item.retry_count << '\t';
            if (const auto* response = std::get_if<fluxcrawl::CrawlResponseMetadata>(&record.outcome)) {
                std::cout << response->status_code << '\t';
                if (response->retry_after) { std::cout << response->retry_after->count(); }
                else { std::cout << '-'; }
            } else { std::cout << "error\t-"; }
            std::cout << '\n';
        }
        std::cout << "summary\t" << result.pages_attempted << '\t' << result.urls_discovered << '\t'
            << result.urls_pending << '\t' << result.retries_scheduled << '\t' << result.retries_attempted << '\t'
            << result.retries_exhausted << '\t' << result.responses_recovered_after_retry << '\t'
            << result.retry_after_used << '\t' << result.retries_cancelled << '\t'
            << result.resources_completed << '\t' << result.terminal_http_failures << '\t'
            << result.terminal_fetch_failures << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
