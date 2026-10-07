#include <fluxcrawl/benchmark.hpp>
#include <iomanip>
#include <locale>
#include <sstream>

namespace fluxcrawl {
namespace {
std::string csv_quote(std::string_view text)
{
    std::string result = "\"";
    for (const char ch : text) {
        if (ch == '"') { result += '"'; }
        result += ch;
    }
    return result + '"';
}
void number(std::ostream& out, const std::optional<double>& value)
{
    if (value) { out << *value; }
}
void latency(std::ostream& out, const std::optional<std::chrono::nanoseconds>& value)
{
    if (value) { out << std::chrono::duration<double, std::milli>(*value).count(); }
}
} // namespace

void write_benchmark_csv(std::ostream& output, const BenchmarkReport& report)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(6);
    out << "version,build_type,seed,workload_pages,fanout,delay_ms,body_bytes,max_depth,max_pages,expected_pages,warmups,"
           "engine,configured_concurrency,run,pages_attempted,responses_received,fetch_failures,bytes_downloaded,"
           "elapsed_ms,attempts_per_second,responses_per_second,bytes_per_second,latency_samples,p50_ms,p95_ms,p99_ms,"
           "server_requests,server_peak_active,scenario,latency_mean_ms,latency_min_ms,latency_max_ms,measured_runs,started_at_unix_ms\n";
    for (const auto& sample : report.samples) {
        out << csv_quote(report.version) << ',' << csv_quote(report.build_type) << ',' << csv_quote(report.seed) << ','
            << report.workload.pages << ',' << report.workload.fanout << ',' << report.workload.delay_ms << ','
            << report.workload.body_bytes << ',' << report.max_depth << ',' << report.max_pages << ','
            << report.expected_pages << ',' << report.warmups << ',' << engine_name(sample.engine) << ','
            << sample.concurrency << ',' << sample.run << ',' << sample.pages_attempted << ','
            << sample.responses_received << ',' << sample.fetch_failures << ',' << sample.bytes_downloaded << ','
            << std::chrono::duration<double, std::milli>(sample.elapsed).count() << ',';
        number(out, sample.metrics.attempts_per_second); out << ',';
        number(out, sample.metrics.responses_per_second); out << ',';
        number(out, sample.metrics.bytes_per_second); out << ',' << sample.metrics.latency.samples << ',';
        latency(out, sample.metrics.latency.p50); out << ',';
        latency(out, sample.metrics.latency.p95); out << ',';
        latency(out, sample.metrics.latency.p99); out << ',';
        out << sample.server_requests << ',' << sample.server_peak_active << ',' << csv_quote(report.workload.scenario) << ',';
        number(out, sample.metrics.latency.mean_ms); out << ',';
        latency(out, sample.metrics.latency.minimum); out << ',';
        latency(out, sample.metrics.latency.maximum);
        out << ',' << report.measured_runs << ',' << report.started_at_unix_ms << '\n';
    }
    output << out.str();
}

void write_benchmark_table(std::ostream& output, const BenchmarkReport& report)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "FluxCrawl " << report.version << " | build: " << report.build_type << '\n'
        << "Scenario: " << report.workload.scenario << "\nWorkload: pages=" << report.workload.pages << " fanout=" << report.workload.fanout
        << " delay_ms=" << report.workload.delay_ms << " body_bytes=" << report.workload.body_bytes
        << " runs=" << report.measured_runs << " expected=" << report.expected_pages << " warmups=" << report.warmups << '\n';
    if (report.build_type != "Release") { out << "Use a Release build for performance comparisons.\n"; }
    out << "Engine       Conc Run  Elapsed_ms Attempts/s Responses/s Bytes/s P50_ms P95_ms P99_ms Mean_ms Min_ms Max_ms\n";
    out << std::fixed << std::setprecision(3);
    for (const auto& sample : report.samples) {
        out << std::left << std::setw(12) << engine_name(sample.engine) << ' ' << std::right
            << std::setw(4) << sample.concurrency << ' ' << std::setw(3) << sample.run << ' '
            << std::chrono::duration<double, std::milli>(sample.elapsed).count() << ' ';
        for (const auto& rate : {sample.metrics.attempts_per_second, sample.metrics.responses_per_second,
                                 sample.metrics.bytes_per_second}) {
            if (rate) { out << *rate; } else { out << "NA"; }
            out << ' ';
        }
        for (const auto& value : {sample.metrics.latency.p50, sample.metrics.latency.p95, sample.metrics.latency.p99}) {
            if (value) { latency(out, value); } else { out << "NA"; }
            out << ' ';
        }
        if (sample.metrics.latency.mean_ms) { number(out, sample.metrics.latency.mean_ms); } else { out << "NA"; }
        out << ' ';
        for (const auto& value : {sample.metrics.latency.minimum, sample.metrics.latency.maximum}) {
            if (value) { latency(out, value); } else { out << "NA"; }
            out << ' ';
        }
        out << '\n';
    }
    out << "Medians across measured runs (request percentiles remain per trial):\n";
    for (const auto& aggregate : report.aggregates) {
        out << engine_name(aggregate.engine) << " conc=" << aggregate.concurrency << " samples=" << aggregate.samples
            << " elapsed_ms=" << aggregate.median_elapsed_ms
            << " elapsed_stddev_ms=" << aggregate.elapsed_stddev_ms << " attempts/s=";
        if (aggregate.median_attempts_per_second) { number(out, aggregate.median_attempts_per_second); }
        else { out << "NA"; }
        out << '\n';
    }
    output << out.str();
}
} // namespace fluxcrawl
