#include <fluxcrawl/version.hpp>

#include <iostream>
#include <string_view>

namespace {

void print_help()
{
    std::cout << "FluxCrawl: a C++20 network-concurrency engineering project.\n"
                 "Under development; crawling is not implemented yet.\n\n"
                 "Usage:\n"
                 "  fluxcrawl [options] <seed-url>\n\n"
                 "Options:\n"
                 "  --help       Show this help message\n"
                 "  --version    Show the current version\n";
}

} // namespace

int main(int argc, char* argv[])
{
    bool show_help = false;
    bool show_version = false;
    bool has_seed = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            show_help = true;
        } else if (argument == "--version") {
            show_version = true;
        } else if (argument.starts_with('-')) {
            std::cerr << "fluxcrawl: unknown option '" << argument
                      << "'. Use --help for usage.\n";
            return 1;
        } else if (argument.empty() || has_seed) {
            std::cerr << "fluxcrawl: expected one non-empty seed URL. Use --help for usage.\n";
            return 1;
        } else {
            has_seed = true;
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
    if (has_seed) {
        std::cerr << "fluxcrawl: crawl engine not implemented yet.\n";
        return 1;
    }

    std::cerr << "fluxcrawl: missing seed URL. Use --help for usage.\n";
    return 1;
}
