#include "benchmark_cli.hpp"
#include "checkpoint_cli.hpp"
#include "persistence_cli.hpp"
#include <fluxcrawl/version.hpp>
#include <fluxcrawl/url.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/async_crawler.hpp>

#ifdef FLUXCRAWL_OBSERVABILITY
#include <fluxcrawl/observability_server.hpp>
#endif
#include <thread>
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
                 "  fluxcrawl resume FILE [execution options]\n"
                 "  fluxcrawl benchmark --help\n\n"
                 "Options:\n"
                 "  --dashboard          Enable read-only loopback dashboard\n"
                 "  --dashboard-port N   Port (default: 8080; 0 chooses a free port)\n"
                 "  --metrics-interval-ms N Sampling interval 100..10000 (default: 500)\n"
                 "  --dashboard-topology Enable bounded host discovery graph\n"
                 "  --dashboard-linger-ms N Keep final state available (default: 2000)\n"
                 "  --state-db FILE       New SQLite crawl database (must not exist)\n"
                 "  --db-batch-size N     Logical mutations per batch (default: 100)\n"
                 "  --dedup MODE          memory (default), sqlite or bloom-sqlite\n"
                 "  --bloom-expected N    Design capacity (default: 1000000)\n"
                 "  --bloom-fp-rate P     Target probability (default: 0.01)\n"
                 "  --seen-reserve N      Reserve MemoryExact capacity\n"
                 "  --pending-high-watermark N  Admission capacity (default: 10000)\n"
                 "  --pending-low-watermark N   Resume admission at this level (default: 7500)\n"
                 "  --unbounded-pending     Disable queue admission control\n"
                 "  --checkpoint FILE      Atomic snapshot output (default: disabled)\n"
                 "  --checkpoint-every N   Resource attempts between snapshots (default: 100)\n"
                 "  --stop-after-attempts N Graceful stop at cumulative attempts; requires checkpoint\n"
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
    if (argc > 1 && std::string_view{argv[1]} == "resume") { return checkpoint_resume_main(argc, argv); }
    bool dashboard = false, dashboard_option = false;
    [[maybe_unused]] bool topology = false;
    [[maybe_unused]] std::size_t dashboard_port = 8080, interval_ms = 500;
    std::size_t linger_ms = 2000;
    bool show_help = false;
    bool bloom_supplied = false;
    bool pending_high_supplied = false, pending_low_supplied = false, pressure_disabled = false;
    bool checkpoint_interval_supplied = false, db_batch_supplied = false;
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
        if (argument == "--dashboard") { dashboard = true;
        } else if (argument == "--dashboard-topology") { topology = true; dashboard_option = true;
        } else if (argument == "--dashboard-port" || argument == "--metrics-interval-ms" || argument == "--dashboard-linger-ms") {
            dashboard_option = true;
            if (++index >= argc) { std::cerr << "fluxcrawl: missing dashboard integer.\n"; return 1; }
            const std::string_view value{argv[index]}; std::size_t number = 0;
            const auto [end, error] = std::from_chars(value.data(),value.data()+value.size(),number);
            if (value.empty() || error!=std::errc{} || end!=value.data()+value.size()
                || (argument=="--dashboard-port" && number>65535)
                || (argument=="--metrics-interval-ms" && (number<100 || number>10000))
                || (argument=="--dashboard-linger-ms" && number>60000)) {
                std::cerr << "fluxcrawl: invalid dashboard integer.\n"; return 1;
            }
            if (argument=="--dashboard-port") { dashboard_port=number; }
            else if (argument=="--metrics-interval-ms") { interval_ms=number; }
            else { linger_ms=number; }
        } else if (argument == "--help") {
            show_help = true;
        } else if (argument == "--version") {
            show_version = true;
        } else if (argument == "--dedup") {
            if (index+1>=argc) { std::cerr<<"fluxcrawl: --dedup needs memory, sqlite or bloom-sqlite.\n";return 1; }
            const std::string_view mode{argv[++index]};
            if(mode=="memory") {options.dedup.mode=fluxcrawl::DedupMode::Memory;}
            else if(mode=="sqlite") {options.dedup.mode=fluxcrawl::DedupMode::Sqlite;}
            else if(mode=="bloom-sqlite") {options.dedup.mode=fluxcrawl::DedupMode::BloomSqlite;}
            else {std::cerr<<"fluxcrawl: unsupported dedup mode.\n";return 1;}
        } else if(argument=="--bloom-expected" || argument=="--seen-reserve") {
            if(index+1>=argc) {std::cerr<<"fluxcrawl: missing dedup count.\n";return 1;}
            const std::string_view value{argv[++index]};std::size_t n=0;const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);
            if(value.empty()||error!=std::errc{}||end!=value.data()+value.size()||n==0){std::cerr<<"fluxcrawl: positive dedup count required.\n";return 1;}
            if(argument=="--bloom-expected"){options.dedup.expected_elements=n;bloom_supplied=true;}else{options.dedup.memory_reserve=n;}
        } else if(argument=="--bloom-fp-rate") {
            if(index+1>=argc){std::cerr<<"fluxcrawl: missing Bloom probability.\n";return 1;}
            const std::string value{argv[++index]};double probability=0;std::istringstream input{value};input.imbue(std::locale::classic());
            if(value.empty() || !(input>>probability) || !(input>>std::ws).eof()){std::cerr<<"fluxcrawl: invalid Bloom probability.\n";return 1;}
            options.dedup.false_positive_rate=probability;bloom_supplied=true;
        } else if (argument == "--unbounded-pending") {
            pressure_disabled = true; options.backpressure.enabled = false;
        } else if (argument == "--pending-high-watermark" || argument == "--pending-low-watermark") {
            if (index + 1 >= argc) { std::cerr << "fluxcrawl: missing pending watermark.\n"; return 1; }
            const std::string_view value{argv[++index]}; std::size_t n = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), n);
            if (value.empty() || error != std::errc{} || end != value.data()+value.size() || (argument == "--pending-high-watermark" && n == 0)) {
                std::cerr << "fluxcrawl: invalid pending watermark.\n"; return 1;
            }
            if (argument == "--pending-high-watermark") { options.backpressure.high_watermark = n; pending_high_supplied = true; }
            else { options.backpressure.low_watermark = n; pending_low_supplied = true; }
        } else if (argument == "--checkpoint" || argument == "--state-db") {
            if (index + 1 >= argc || std::string_view{argv[index + 1]}.empty()) {
                std::cerr << "fluxcrawl: --checkpoint requires a path.\n"; return 1;
            }
            if (argument == "--state-db") { options.sqlite.path = argv[++index]; }
            else { options.checkpoint.path = argv[++index]; }
        } else if (argument == "--checkpoint-every" || argument == "--stop-after-attempts" || argument == "--db-batch-size") {
            if (index + 1 >= argc) { std::cerr << "fluxcrawl: missing checkpoint integer.\n"; return 1; }
            const std::string_view value{argv[++index]};
            std::size_t n = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (value.empty() || error != std::errc{} || end != value.data() + value.size()
                || (n == 0 && argument != "--stop-after-attempts")) {
                std::cerr << "fluxcrawl: invalid checkpoint integer.\n"; return 1;
            }
            if (argument == "--checkpoint-every") { options.checkpoint.every = n; checkpoint_interval_supplied = true; }
            else if (argument == "--db-batch-size") { options.sqlite.batch_size = n; db_batch_supplied = true; }
            else { options.checkpoint.stop_after_attempts = n; }
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

    if (dashboard_option && !dashboard) { std::cerr << "fluxcrawl: dashboard options require --dashboard.\n"; return 1; }
#ifndef FLUXCRAWL_OBSERVABILITY
    if (dashboard) { std::cerr << "fluxcrawl: dashboard was disabled at build time.\n"; return 1; }
#endif
    try { options.dedup.validate(); } catch(const std::exception& error) {std::cerr<<"fluxcrawl: "<<error.what()<<'\n';return 1;}
    if((bloom_supplied && options.dedup.mode!=fluxcrawl::DedupMode::BloomSqlite) || (options.dedup.mode!=fluxcrawl::DedupMode::Memory && options.sqlite.path.empty())) {
        std::cerr<<"fluxcrawl: Bloom options require bloom-sqlite; SQLite dedup requires --state-db.\n";return 1;
    }
    if (pressure_disabled && (pending_high_supplied || pending_low_supplied)) { std::cerr << "fluxcrawl: unbounded pending conflicts with watermarks.\n"; return 1; }
    if (pending_high_supplied && !pending_low_supplied) { options.backpressure.low_watermark = options.backpressure.high_watermark - (options.backpressure.high_watermark / 4) - (options.backpressure.high_watermark % 4 != 0); }
    try { options.backpressure.validate(); } catch (const std::exception& error) { std::cerr << "fluxcrawl: " << error.what() << '\n'; return 1; }
    if ((!options.sqlite.path.empty() && !options.checkpoint.path.empty())
        || (checkpoint_interval_supplied && options.checkpoint.path.empty())
        || (db_batch_supplied && options.sqlite.path.empty())
        || (options.checkpoint.stop_after_attempts && options.checkpoint.path.empty() && options.sqlite.path.empty())) {
        std::cerr << "fluxcrawl: choose one persistence backend; cadence/batch/stop options require their backend.\n"; return 1;
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
#ifdef FLUXCRAWL_OBSERVABILITY
            std::unique_ptr<fluxcrawl::ObservabilityServer> server;
            if (dashboard) {
                options.observability=std::make_shared<fluxcrawl::Observability>(std::chrono::milliseconds{interval_ms},topology);
                server=std::make_unique<fluxcrawl::ObservabilityServer>(options.observability,static_cast<std::uint16_t>(dashboard_port));
                std::cout << "Dashboard: http://127.0.0.1:" << server->port() << std::endl;
            }
#endif
            const auto result = [&] {
                try {
                if (engine == "async") { return fluxcrawl::AsyncCrawler{concurrency}.crawl(*url, options); }
                if (engine == "threaded") { return fluxcrawl::ThreadPoolCrawler{workers}.crawl(*url, options); }
                return fluxcrawl::SequentialCrawler{}.crawl(*url, options);
                } catch (...) {
#ifdef FLUXCRAWL_OBSERVABILITY
                    if (options.observability) { options.observability->failed(); std::this_thread::sleep_for(std::chrono::milliseconds{linger_ms}); }
#endif
                    throw;
                }
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
                      << "\n\n" << (result.urls_pending == 0 && result.pressure.deferred_urls == 0 ? "Frontier exhausted" : options.checkpoint.stop_after_attempts && result.pages_attempted >= *options.checkpoint.stop_after_attempts ? "Checkpoint stop reached" : "Page limit reached")
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
            if (result.pages_attempted != 0) { fluxcrawl::detail::print_dedup_metrics(std::cout, result, options.dedup); }
            if (result.pages_attempted != 0) { fluxcrawl::detail::print_pressure_metrics(std::cout, result, options.backpressure); }
            if (!options.checkpoint.path.empty()) {
                std::cout << "Checkpoints written: " << result.checkpoints_written
                    << "\nCheckpoint bytes: " << result.checkpoint_bytes
                    << "\nCheckpoint I/O time ms: " << std::chrono::duration<double, std::milli>(result.checkpoint_time).count() << '\n';
            }
            if (!options.sqlite.path.empty()) { fluxcrawl::detail::print_database_metrics(std::cout, result); }
            if (dashboard) { std::this_thread::sleep_for(std::chrono::milliseconds{linger_ms}); }
            return result.persistence_errors.empty() && result.checkpoint_errors.empty() && result.robots_fetch_failures == 0 && result.terminal_fetch_failures == 0
                && (options.retry.max_retries == 0 || result.terminal_http_failures == 0) ? 0 : 1;
        } catch (const std::exception& error) {
            std::cerr << "fluxcrawl: " << error.what() << '\n';
            return 1;
        }
    }

    std::cerr << "fluxcrawl: missing seed URL. Use --help for usage.\n";
    return 1;
}
