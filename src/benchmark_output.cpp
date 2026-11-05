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
double milliseconds(ProfileDuration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}
void profile_csv(std::ostream& out, const std::optional<ProfileSummary>& profile)
{
    out << ',' << (profile ? 1 : 0);
    if (!profile) { out << std::string(23, ','); return; }
    const auto& common = profile->common;
    out << ',' << milliseconds(common.extraction) << ',' << milliseconds(common.discovery)
        << ',' << milliseconds(common.frontier) << ',' << common.extractions
        << ',' << common.links_considered << ',' << common.links_accepted;
    if (profile->threaded) {
        const auto& threaded = *profile->threaded;
        const auto& total = threaded.total;
        out << ',' << milliseconds(total.lock_wait) << ',' << milliseconds(total.idle)
            << ',' << milliseconds(total.activity) << ',' << total.lock_acquisitions
            << ',' << total.waits << ',' << total.wakeups << ',' << total.requests << ',' << threaded.max_active;
    } else { out << std::string(8, ','); }
    if (profile->async) {
        const auto& async = *profile->async;
        out << ',' << milliseconds(async.multi_wait) << ',' << milliseconds(async.completions_time)
            << ',' << milliseconds(async.refill_time) << ',' << async.iterations << ',' << async.waits
            << ',' << async.completions << ',' << async.refills << ',' << async.max_active;
    } else { out << std::string(8, ','); }
    // Number of worker records distinguishes no workers launched from an absent engine section.
    out << ',';
    if (profile->threaded) { out << profile->threaded->workers.size(); }
}
void profile_table(std::ostream& out, const ProfileSummary& profile)
{
    const auto& common = profile.common;
    out << "  Profile (overlapping wall durations, ms): extraction=" << milliseconds(common.extraction)
        << " discovery=" << milliseconds(common.discovery) << " frontier=" << milliseconds(common.frontier)
        << " extractions=" << common.extractions << " considered=" << common.links_considered
        << " accepted=" << common.links_accepted << '\n';
    out << "  Retry decisions=" << common.retry_decisions
        << " scheduled delay_ms=" << milliseconds(common.retry_delay) << '\n';
    if (profile.threaded) {
        const auto& threaded = *profile.threaded;
        const auto& total = threaded.total;
        out << "    Workers: lock_wait=" << milliseconds(total.lock_wait) << " idle=" << milliseconds(total.idle)
            << " activity=" << milliseconds(total.activity) << " acquisitions=" << total.lock_acquisitions
            << " waits=" << total.waits << " wakeups=" << total.wakeups << " requests=" << total.requests
            << " max_active=" << threaded.max_active << '\n';
        for (std::size_t i = 0; i < threaded.workers.size(); ++i) {
            const auto& worker = threaded.workers[i];
            out << "      worker=" << i << " requests=" << worker.requests
                << " activity_ms=" << milliseconds(worker.activity) << " idle_ms=" << milliseconds(worker.idle) << '\n';
        }
    }
    if (profile.async) {
        const auto& async = *profile.async;
        out << "    Event loop: multi_wait=" << milliseconds(async.multi_wait)
            << " completion_processing=" << milliseconds(async.completions_time)
            << " refill=" << milliseconds(async.refill_time) << " iterations=" << async.iterations
            << " waits=" << async.waits << " completions=" << async.completions
            << " refills=" << async.refills << " max_active=" << async.max_active << '\n';
    }
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
           "server_requests,server_peak_active,scenario,latency_mean_ms,latency_min_ms,latency_max_ms,measured_runs,started_at_unix_ms,"
           "shared_anchors,delay_pattern,profile_enabled,extraction_ms,discovery_ms,frontier_ms,extractions,links_considered,links_accepted,"
           "lock_wait_ms,worker_idle_ms,worker_activity_ms,lock_acquisitions,cv_waits,worker_wakeups,worker_requests,max_active_workers,"
           "multi_wait_ms,completion_processing_ms,refill_ms,event_loop_iterations,multi_waits,completions,refills,max_active_transfers,worker_profiles\n";
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
        out << ',' << report.measured_runs << ',' << report.started_at_unix_ms
            << ',' << report.workload.shared_anchors << ',' << csv_quote(report.workload.delay_pattern);
        profile_csv(out, sample.profile);
        out << '\n';
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
        << " shared_anchors=" << report.workload.shared_anchors << " delay_pattern=" << report.workload.delay_pattern
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
        if (sample.profile) { profile_table(out, *sample.profile); }
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
