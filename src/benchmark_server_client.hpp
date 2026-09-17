#pragma once
#include <fluxcrawl/benchmark.hpp>
#include <fluxcrawl/http_client.hpp>

namespace fluxcrawl::detail {
struct ServerStats { std::size_t requests = 0, active = 0, peak = 0, unique = 0; };
// Control-plane requests only; none are included in crawl timing or byte totals.
class BenchmarkServerClient {
public:
    explicit BenchmarkServerClient(const Url& seed);
    BenchmarkWorkload workload() const;
    void reset() const;
    ServerStats stats() const;
private:
    std::string get(std::string_view path) const;
    Url seed_;
    HttpClient client_;
};
HttpClientOptions benchmark_http_options();
} // namespace fluxcrawl::detail
