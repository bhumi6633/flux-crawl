#include "benchmark_cli.hpp"
#include <fluxcrawl/benchmark.hpp>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::size_t integer(std::string_view value, std::string_view option)
{
    std::size_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument(std::string{option} + " requires a non-negative integer");
    }
    return number;
}
std::vector<std::string_view> split(std::string_view list)
{
    std::vector<std::string_view> values;
    while (true) {
        const auto end = list.find(',');
        const auto value = list.substr(0, end);
        if (value.empty()) { throw std::invalid_argument("Benchmark list must not contain empty entries"); }
        values.push_back(value);
        if (end == std::string_view::npos) { return values; }
        list.remove_prefix(end + 1);
    }
}
class CsvFile {
public:
    explicit CsvFile(const std::string& path) : path_{path}
    {
        if (!path.empty()) {
            file_ = std::fopen(path.c_str(), "wx"); // Exclusive creation; never truncate an existing file.
            if (!file_) { throw std::runtime_error("Cannot create CSV output (must be a new writable file): " + path); }
        }
    }
    ~CsvFile()
    {
        if (file_) { std::fclose(file_); }
        if (!path_.empty() && !complete_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    CsvFile(const CsvFile&) = delete;
    CsvFile& operator=(const CsvFile&) = delete;
    void write(const fluxcrawl::BenchmarkReport& report)
    {
        if (!file_) { return; }
        std::ostringstream text;
        fluxcrawl::write_benchmark_csv(text, report);
        const auto bytes = text.str();
        if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
            throw std::runtime_error("Failed writing benchmark CSV");
        }
        auto* closing = file_;
        file_ = nullptr;
        if (std::fclose(closing) != 0) { throw std::runtime_error("Failed closing benchmark CSV"); }
        complete_ = true;
    }
private:
    std::string path_;
    std::FILE* file_ = nullptr;
    bool complete_ = false;
};
} // namespace

int benchmark_main(int argc, char* argv[])
{
    try {
        auto seed = fluxcrawl::Url::parse("http://127.0.0.1:8080/page/0").value();
        fluxcrawl::BenchmarkConfig config{seed};
        std::string output;
        bool has_seed = false, help = false;
        for (int index = 2; index < argc; ++index) {
            const std::string_view option{argv[index]};
            if (option == "--help") { help = true; continue; }
            if (option != "--seed" && option != "--engines" && option != "--concurrency" && option != "--depth"
                && option != "--max-pages" && option != "--runs" && option != "--warmups" && option != "--output") {
                throw std::invalid_argument("Unknown benchmark option: " + std::string{option});
            }
            if (index + 1 >= argc) { throw std::invalid_argument("Missing value for " + std::string{option}); }
            const std::string_view value{argv[++index]};
            if (option == "--seed") {
                const auto parsed = fluxcrawl::Url::parse(value);
                if (!parsed) { throw std::invalid_argument("Invalid benchmark seed URL"); }
                config.seed = *parsed;
                has_seed = true;
            } else if (option == "--engines") {
                config.engines.clear();
                for (const auto name : split(value)) {
                    if (name == "sequential") { config.engines.push_back(fluxcrawl::EngineType::Sequential); }
                    else if (name == "threaded") { config.engines.push_back(fluxcrawl::EngineType::Threaded); }
                    else if (name == "async") { config.engines.push_back(fluxcrawl::EngineType::Async); }
                    else { throw std::invalid_argument("Unknown benchmark engine: " + std::string{name}); }
                }
            } else if (option == "--concurrency") {
                config.concurrency.clear();
                for (const auto item : split(value)) {
                    const auto number = integer(item, option);
                    if (number == 0) { throw std::invalid_argument("Benchmark concurrency must be positive"); }
                    config.concurrency.push_back(number);
                }
            } else if (option == "--output") {
                if (value.empty()) { throw std::invalid_argument("Empty CSV output path"); }
                output = value;
            } else {
                const auto number = integer(value, option);
                if (option == "--runs") {
                    if (number == 0) { throw std::invalid_argument("Benchmark runs must be positive"); }
                    config.runs = number;
                } else if (option == "--warmups") { config.warmups = number; }
                else if (option == "--depth") { config.crawl.max_depth = number; }
                else { config.crawl.max_pages = number; }
            }
        }
        if (help) {
            std::cout << "Usage: fluxcrawl benchmark --seed http://127.0.0.1:<port>/page/0 [options]\n"
                         "Start tools/benchmark_server.py separately; benchmark traffic stays on loopback.\n"
                         "  --engines LIST     sequential,threaded,async (default: all three)\n"
                         "  --concurrency LIST Positive levels (default: 1,2,4,8); sequential always 1\n"
                         "  --depth N          Inclusive depth (default: 20)\n"
                         "  --max-pages N      Must cover depth-reachable graph, or 0 (default: 100)\n"
                         "  --runs N           Measured repetitions (default: 3; positive)\n"
                         "  --warmups N        Excluded warmup sweeps (default: 0)\n"
                         "  --output PATH      CSV; exclusively creates a new file, never overwrites\n"
                         "  --help             Show benchmark help\n"
                         "Threaded concurrency counts workers; async counts active transfers.\n"
                         "Metrics: active request P50/P95/P99, crawl elapsed, attempts/s, responses/s, bytes/s.\n"
                         "Use a Release build for performance comparisons.\n";
            return 0;
        }
        if (!has_seed) { throw std::invalid_argument("Benchmark requires --seed"); }
        CsvFile csv{output};
        const auto report = fluxcrawl::BenchmarkRunner{}.run(config);
        csv.write(report);
        fluxcrawl::write_benchmark_table(std::cout, report);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fluxcrawl benchmark: " << error.what() << '\n';
        return 1;
    }
}
