#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <iostream>
#include <set>
#include <stdexcept>

// Machine-readable driver for Python's shared multi-origin fixture. This also
// verifies library accounting and outcomes, which the CLI summary cannot expose.
int main(int argc, char** argv)
{
    if (argc != 11) { return 2; }
    try {
        const auto seed = fluxcrawl::Url::parse(argv[1]).value();
        const std::string engine{argv[2]};
        const auto global = std::stoull(argv[3]);
        fluxcrawl::CrawlOptions options;
        const auto cap = std::stoull(argv[4]);
        const auto rate = std::stod(argv[5]);
        if (cap) { options.scheduling.max_per_host = cap; }
        if (rate != 0) { options.scheduling.rate = fluxcrawl::RateLimit{rate, std::stoull(argv[6])}; }
        options.max_pages = std::stoull(argv[7]);
        options.max_depth = std::stoull(argv[8]);
        options.same_host_only = std::string{argv[9]} == "1";
        fluxcrawl::HttpClientOptions http;
        http.max_body_bytes = std::stoull(argv[10]);
        http.use_environment_proxy = false;
        http.connect_timeout = std::chrono::seconds{2};
        http.request_timeout = std::chrono::seconds{10};
        const auto result = [&] {
            if (engine == "sequential") { return fluxcrawl::SequentialCrawler{http}.crawl(seed, options); }
            if (engine == "threaded") { return fluxcrawl::ThreadPoolCrawler{global, http}.crawl(seed, options); }
            if (engine == "async") { return fluxcrawl::AsyncCrawler{global, http}.crawl(seed, options); }
            throw std::invalid_argument("Unknown engine");
        }();
        if (result.pages_attempted > options.max_pages || result.pages_attempted != result.records.size()
            || result.pages_attempted != result.responses_received + result.fetch_failures
            || result.urls_discovered != result.pages_attempted + result.urls_pending) {
            throw std::runtime_error("Inconsistent crawl accounting");
        }
        std::set<std::string> seen;
        for (const auto& record : result.records) {
            if (!seen.insert(record.item.url.str()).second || record.item.depth > options.max_depth) {
                throw std::runtime_error("Duplicate fetch or invalid depth");
            }
            std::cout << record.item.url.str() << '\t' << record.item.depth << '\t';
            if (const auto* response = std::get_if<fluxcrawl::CrawlResponseMetadata>(&record.outcome)) {
                std::cout << response->status_code << '\t' << response->effective_url.str();
            } else { std::cout << "error\t-"; }
            std::cout << '\n';
        }
        std::cout << "summary\t" << result.pages_attempted << '\t' << result.fetch_failures << '\t' << result.urls_pending << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
