#include "benchmark_server_client.hpp"
#include <locale>
#include <sstream>
#include <stdexcept>

namespace fluxcrawl::detail {
HttpClientOptions benchmark_http_options()
{
    HttpClientOptions options;
    options.use_environment_proxy = false;
    options.max_redirects = 0;
    return options;
}
BenchmarkServerClient::BenchmarkServerClient(const Url& seed) : seed_{seed}, client_{benchmark_http_options()}
{
    if (seed.scheme() != "http" || seed.host() != "127.0.0.1" || seed.path() != "/page/0" || seed.has_query()) {
        throw std::invalid_argument("Benchmark seed must be http://127.0.0.1:<port>/page/0 on the benchmark server");
    }
}
std::string BenchmarkServerClient::get(std::string_view path) const
{
    const auto url = Url::resolve(seed_, path).value();
    auto result = client_.fetch(url);
    const auto* response = std::get_if<HttpResponse>(&result);
    if (!response || response->status_code != 200) {
        throw std::runtime_error("Benchmark server control request failed: " + std::string{path});
    }
    return response->body;
}
BenchmarkWorkload BenchmarkServerClient::workload() const
{
    std::istringstream input{get("/__benchmark/config")};
    input.imbue(std::locale::classic());
    std::string protocol;
    BenchmarkWorkload workload;
    if (!(input >> protocol >> workload.pages >> workload.fanout >> workload.delay_ms >> workload.body_bytes)
        || (protocol != "fluxcrawl-benchmark-v1" && protocol != "fluxcrawl-benchmark-v2") || workload.pages == 0 || workload.pages > 100000
        || workload.fanout == 0 || workload.fanout > 1000 || workload.delay_ms > 10000
        || workload.body_bytes == 0 || workload.body_bytes > 1048576) {
        throw std::runtime_error("Invalid benchmark server workload metadata");
    }
    if (protocol == "fluxcrawl-benchmark-v2") {
        if (!(input >> workload.scenario) || workload.scenario.size() > 64
            || workload.scenario.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) {
            throw std::runtime_error("Invalid benchmark scenario metadata");
        }
    }
    input >> std::ws;
    if (!input.eof()) { throw std::runtime_error("Unexpected benchmark workload fields"); }
    return workload;
}
void BenchmarkServerClient::reset() const
{
    if (get("/__benchmark/reset") != "ok\n") { throw std::runtime_error("Benchmark reset failed"); }
}
ServerStats BenchmarkServerClient::stats() const
{
    std::istringstream input{get("/__benchmark/stats")};
    input.imbue(std::locale::classic());
    ServerStats stats;
    if (!(input >> stats.requests >> stats.active >> stats.peak >> stats.unique)) {
        throw std::runtime_error("Invalid benchmark server counters");
    }
    input >> std::ws;
    if (!input.eof()) { throw std::runtime_error("Unexpected benchmark counter fields"); }
    return stats;
}
} // namespace fluxcrawl::detail
