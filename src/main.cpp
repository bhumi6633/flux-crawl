#include <fluxcrawl/version.hpp>
#include <fluxcrawl/url.hpp>
#include <fluxcrawl/sequential_crawler.hpp>

#include <charconv>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

void print_help()
{
    std::cout << "FluxCrawl: a C++20 network-concurrency engineering project.\n"
                 "Sequential blocking HTTP/HTTPS crawler.\n\n"
                 "Usage:\n"
                 "  fluxcrawl [options] <seed-url>\n\n"
                 "Options:\n"
                 "  --depth N       Maximum crawl depth (default: 2)\n"
                 "  --max-pages N   Maximum fetch attempts (default: 100)\n"
                 "  --same-host     Accept discoveries matching the seed host\n"
                 "  --help       Show this help message\n"
                 "  --version    Show the current version\n";
}

} // namespace

int main(int argc, char* argv[])
{
    bool show_help = false;
    bool show_version = false;
    std::string_view seed;
    fluxcrawl::CrawlOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            show_help = true;
        } else if (argument == "--version") {
            show_version = true;
        } else if (argument == "--same-host") {
            options.same_host_only = true;
        } else if (argument == "--depth" || argument == "--max-pages") {
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
            if (argument == "--depth") { options.max_depth = number; }
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
            const fluxcrawl::SequentialCrawler crawler;
            const auto result = crawler.crawl(*url, options);
            std::cout << "Seed: " << url->canonical()
                      << "\nMax depth: " << options.max_depth
                      << "\nMax pages: " << options.max_pages
                      << "\n\n" << (result.urls_pending == 0 ? "Frontier exhausted" : "Page limit reached")
                      << "\nPages attempted: " << result.pages_attempted
                      << "\nResponses: " << result.responses_received
                      << "\nHTML pages processed: " << result.html_pages_processed
                      << "\nFetch failures: " << result.fetch_failures
                      << "\nURLs discovered: " << result.urls_discovered
                      << "\nURLs pending: " << result.urls_pending << '\n';
            return result.fetch_failures == 0 ? 0 : 1;
        } catch (const std::exception& error) {
            std::cerr << "fluxcrawl: " << error.what() << '\n';
            return 1;
        }
    }

    std::cerr << "fluxcrawl: missing seed URL. Use --help for usage.\n";
    return 1;
}
