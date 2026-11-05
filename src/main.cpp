#include "benchmark_cli.hpp"
#include <fluxcrawl/version.hpp>
#include <fluxcrawl/url.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/async_crawler.hpp>

#include <charconv>
#include <limits>
#include <exception>
#include <cmath>
#include <sstream>
#include <locale>
#include <iostream>
#include <string_view>

namespace {

void print_help()
{
    std::cout << "FluxCrawl: a C++20 network-concurrency engineering project.\n"
                 "Sequential, threaded, and event-driven HTTP/HTTPS crawler.\n\n"
                 "Usage:\n"
                 "  fluxcrawl [options] <seed-url>\n"
                 "  fluxcrawl benchmark --help\n\n"
                 "Options:\n"
                 "  --engine NAME   sequential (default), threaded, or async\n"
                 "  --workers N     Threaded workers (default: 4; must be positive)\n"
                 "  --concurrency N Async simultaneous transfers (default: 8; positive)\n"
                 "                  One event-loop thread, continuous slot refill\n"
                 "  --max-per-host N   Maximum active claims per origin (default: unlimited)\n"
                 "  --rate-per-host R  Positive requests/sec per origin (default: disabled)\n"
                 "  --burst-per-host N Token capacity (default: 1; requires --rate-per-host)\n"
                 "  --respect-robots / --ignore-robots  Origin policy (default: ignore)\n"
                 "  --max-retries N  Retries after initial attempt (default: 0, disabled)\n"
                 "  --retry-backoff-ms N      Initial delay (default: 250, positive)\n"
                 "  --retry-max-backoff-ms N  Delay cap (default: 5000, >= initial)\n"
                 "  --retry-jitter / --no-retry-jitter  Full jitter (default: enabled)\n"
                 "  --depth N       Maximum crawl depth (default: 2)\n"
                 "  --max-pages N   Maximum fetch attempts (default: 100)\n"
                 "  --same-host     Accept discoveries matching the seed host\n"
                 "  --help       Show this help message\n"
                 "  --version    Show the current version\n";
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1 && std::string_view{argv[1]} == "benchmark") { return benchmark_main(argc, argv); }
    bool show_help = false;
    bool show_version = false;
    std::string_view seed;
    fluxcrawl::CrawlOptions options;
    std::string_view engine = "sequential";
    std::size_t workers = 4;
    bool workers_supplied = false;
    std::size_t concurrency = 8;
    bool concurrency_supplied = false;
    std::size_t burst = 1;
    bool burst_supplied = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            show_help = true;
        } else if (argument == "--version") {
            show_version = true;
        } else if (argument == "--same-host") {
            options.same_host_only = true;
        } else if (argument == "--respect-robots" || argument == "--ignore-robots") {
            options.respect_robots = argument == "--respect-robots";
        } else if (argument == "--retry-jitter" || argument == "--no-retry-jitter") {
            options.retry.jitter = argument == "--retry-jitter";
        } else if (argument == "--max-retries" || argument == "--retry-backoff-ms" || argument == "--retry-max-backoff-ms") {
            if (index + 1 >= argc) {
                std::cerr << "fluxcrawl: " << argument << " requires an integer.\n";
                return 1;
            }
            const std::string_view value{argv[++index]};
            std::size_t number = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (value.empty() || error != std::errc{} || end != value.data() + value.size()
                || (argument != "--max-retries" && (number == 0
                    || number > static_cast<std::size_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())))) {
                std::cerr << "fluxcrawl: invalid integer for " << argument << ".\n";
                return 1;
            }
            if (argument == "--max-retries") { options.retry.max_retries = number; }
            else if (argument == "--retry-backoff-ms") { options.retry.initial_backoff = std::chrono::milliseconds{number}; }
            else { options.retry.max_backoff = std::chrono::milliseconds{number}; }
        } else if (argument == "--rate-per-host") {
            if (index + 1 >= argc) {
                std::cerr << "fluxcrawl: --rate-per-host requires a finite positive number.\n";
                return 1;
            }
            const std::string value{argv[++index]};
            std::istringstream input{value};
            input.imbue(std::locale::classic());
            double rate = 0;
            input >> std::noskipws >> rate;
            if (!input || !input.eof() || !std::isfinite(rate) || rate <= 0) {
                std::cerr << "fluxcrawl: --rate-per-host requires a finite positive number.\n";
                return 1;
            }
            options.scheduling.rate = fluxcrawl::RateLimit{rate, burst};
        } else if (argument == "--engine") {
            if (index + 1 >= argc) {
                std::cerr << "fluxcrawl: --engine requires sequential, threaded, or async.\n";
                return 1;
            }
            engine = argv[++index];
            if (engine != "sequential" && engine != "threaded" && engine != "async") {
                std::cerr << "fluxcrawl: --engine requires sequential, threaded, or async.\n";
                return 1;
            }
        } else if (argument == "--depth" || argument == "--max-pages" || argument == "--workers" || argument == "--concurrency" || argument == "--max-per-host" || argument == "--burst-per-host") {
            if (index + 1 >= argc) {
                std::cerr << "fluxcrawl: " << argument << " requires a non-negative integer.\n";
                return 1;
            }
            const std::string_view value{argv[++index]};
            std::size_t number = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
                std::cerr << "fluxcrawl: " << argument << " requires a non-negative integer.\n";
                return 1;
            }
            if (argument == "--workers" || argument == "--concurrency" || argument == "--max-per-host" || argument == "--burst-per-host") {
                if (number == 0) {
                    std::cerr << "fluxcrawl: " << argument << " must be greater than zero.\n";
                    return 1;
                }
                if (argument == "--max-per-host") {
                    options.scheduling.max_per_host = number;
                } else if (argument == "--burst-per-host") {
                    burst = number;
                    burst_supplied = true;
                } else if (argument == "--workers") {
                    workers = number;
                    workers_supplied = true;
                } else {
                    concurrency = number;
                    concurrency_supplied = true;
                }
            } else if (argument == "--depth") { options.max_depth = number; }
            else { options.max_pages = number; }
        } else if (argument.starts_with('-')) {
            std::cerr << "fluxcrawl: unknown option '" << argument
                      << "'. Use --help for usage.\n";
            return 1;
        } else if (argument.empty() || !seed.empty()) {
            std::cerr << "fluxcrawl: expected one non-empty seed URL. Use --help for usage.\n";
            return 1;
        } else {
            seed = argument;
        }
    }

    if (burst_supplied && !options.scheduling.rate) {
        std::cerr << "fluxcrawl: --burst-per-host requires --rate-per-host.\n";
        return 1;
    }
    if (options.scheduling.rate) { options.scheduling.rate->burst = burst; }
    if (workers_supplied && engine != "threaded") {
        std::cerr << "fluxcrawl: --workers requires --engine threaded.\n";
        return 1;
    }
    if (concurrency_supplied && engine != "async") {
        std::cerr << "fluxcrawl: --concurrency requires --engine async.\n";
        return 1;
    }
    if (show_help) {
        print_help();
        return 0;
    }
    if (show_version) {
        std::cout << "FluxCrawl " << fluxcrawl::version << '\n';
        return 0;
    }
    if (!seed.empty()) {
        const auto url = fluxcrawl::Url::parse(seed);
        if (!url) {
            std::cerr << "fluxcrawl: invalid or unsupported seed URL; expected a valid HTTP/HTTPS URL.\n";
            return 1;
        }
        try {
            const auto result = [&] {
                if (engine == "async") { return fluxcrawl::AsyncCrawler{concurrency}.crawl(*url, options); }
                if (engine == "threaded") { return fluxcrawl::ThreadPoolCrawler{workers}.crawl(*url, options); }
                return fluxcrawl::SequentialCrawler{}.crawl(*url, options);
            }();
            std::cout << "Seed: " << url->canonical() << "\nEngine: " << engine;
            if (engine == "threaded") { std::cout << "\nWorkers: " << workers; }
            if (engine == "async") { std::cout << "\nConcurrency: " << concurrency; }
            std::cout << "\nMax per host: ";
            if (options.scheduling.max_per_host) { std::cout << *options.scheduling.max_per_host; }
            else { std::cout << "unlimited"; }
            std::cout << "\nRate per host: ";
            if (options.scheduling.rate) {
                std::cout << options.scheduling.rate->requests_per_second
                          << " requests/s; burst " << options.scheduling.rate->burst;
            } else { std::cout << "disabled"; }
            std::cout
                      << "\nMax depth: " << options.max_depth
                      << "\nMax pages: " << options.max_pages
                      << "\n\n" << (result.urls_pending == 0 ? "Frontier exhausted" : "Page limit reached")
                      << "\nPages attempted: " << result.pages_attempted
                      << "\nResponses: " << result.responses_received
                      << "\nHTML pages processed: " << result.html_pages_processed
                      << "\nFetch failures: " << result.fetch_failures
                      << "\nURLs discovered: " << result.urls_discovered
                      << "\nURLs pending: " << result.urls_pending << '\n';
            if (options.respect_robots) {
                std::cout << "Robots fetches: " << result.robots_fetches
                    << "\nRobots origins: " << result.robots_origins
                    << "\nRobots cache hits: " << result.robots_cache_hits
                    << "\nRobots allowed: " << result.robots_allowed
                    << "\nURLs blocked by robots: " << result.urls_blocked_by_robots
                    << "\nRobots fetch failures: " << result.robots_fetch_failures
                    << "\nRobots retries attempted: " << result.robots_retries_attempted
                    << "\nRobots retries cancelled: " << result.robots_retries_cancelled
                    << "\nRobots responses reused: " << result.robots_resources_reused << '\n';
            }
            if (options.retry.max_retries != 0) {
                std::cout << "Retries scheduled: " << result.retries_scheduled
                    << "\nRetries attempted: " << result.retries_attempted
                    << "\nRecovered after retry: " << result.responses_recovered_after_retry
                    << "\nRetries exhausted: " << result.retries_exhausted
                    << "\nRetry-After used: " << result.retry_after_used
                    << "\nRetries cancelled by page limit: " << result.retries_cancelled
                    << "\nResources completed: " << result.resources_completed
                    << "\nTerminal HTTP failures: " << result.terminal_http_failures
                    << "\nTerminal fetch failures: " << result.terminal_fetch_failures << '\n';
            }
            return result.robots_fetch_failures == 0 && result.terminal_fetch_failures == 0
                && (options.retry.max_retries == 0 || result.terminal_http_failures == 0) ? 0 : 1;
        } catch (const std::exception& error) {
            std::cerr << "fluxcrawl: " << error.what() << '\n';
            return 1;
        }
    }

    std::cerr << "fluxcrawl: missing seed URL. Use --help for usage.\n";
    return 1;
}
