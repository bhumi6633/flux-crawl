#include <fluxcrawl/benchmark.hpp>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>

using namespace std::chrono_literals;
struct CommaPunctuation : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
int main(int argc, char* argv[])
{
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { if (!ok) { ++failures; std::cerr << label << '\n'; } };
    {
        fluxcrawl::BenchmarkConfig retrying{fluxcrawl::Url::parse("http://127.0.0.1:1/page/0").value()};
        retrying.crawl.retry.max_retries = 1;
        bool rejected = false;
        try { (void)fluxcrawl::BenchmarkRunner{}.run(retrying); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "baseline benchmark rejects retries before contacting fixture");
    }
    {
        fluxcrawl::BenchmarkConfig robots{fluxcrawl::Url::parse("http://127.0.0.1:1/page/0").value()};
        robots.crawl.respect_robots = true;
        bool rejected = false;
        try { (void)fluxcrawl::BenchmarkRunner{}.run(robots); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "baseline benchmark rejects robots bootstrap before network access");
    }
    {
        fluxcrawl::BenchmarkConfig checkpoint{fluxcrawl::Url::parse("http://127.0.0.1:1/page/0").value()};
        checkpoint.crawl.checkpoint.path = "unused.chk";
        bool rejected = false;
        try { (void)fluxcrawl::BenchmarkRunner{}.run(checkpoint); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "baseline benchmark rejects checkpoint I/O before fixture contact");
    }
    {
        fluxcrawl::BenchmarkConfig sqlite{fluxcrawl::Url::parse("http://127.0.0.1:1/page/0").value()};
        sqlite.crawl.sqlite.path = "unused.db";
        bool rejected = false;
        try { (void)fluxcrawl::BenchmarkRunner{}.run(sqlite); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "baseline benchmark rejects SQLite before fixture contact");
    }
    for (bool rate : {false, true}) {
        fluxcrawl::BenchmarkConfig limited{fluxcrawl::Url::parse("http://127.0.0.1:1/page/0").value()};
        if (rate) { limited.crawl.scheduling.rate = fluxcrawl::RateLimit{1, 1}; }
        else { limited.crawl.scheduling.max_per_host = 2; }
        bool rejected = false;
        try { (void)fluxcrawl::BenchmarkRunner{}.run(limited); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "legacy benchmark rejects unrecorded host limits before contacting server");
    }
    fluxcrawl::BenchmarkReport report;
    report.version = "v\"1";
    report.build_type = "Test";
    report.seed = "http://127.0.0.1/page/0";
    report.workload = {10, 2, 5, 100};
    report.max_depth = 20; report.max_pages = 100; report.expected_pages = 10;
    fluxcrawl::BenchmarkSample sample;
    sample.pages_attempted = 100; sample.responses_received = 80; sample.fetch_failures = 20;
    sample.bytes_downloaded = 1000; sample.elapsed = 2s;
    sample.metrics.attempts_per_second = 50;
    sample.metrics.responses_per_second = 40;
    sample.metrics.bytes_per_second = 500;
    sample.metrics.latency = {100, 10ms, 20ms, 30ms, 15.5, 1ms, 40ms};
    sample.server_requests = 100; sample.server_peak_active = 1;
    report.samples.clear();
    report.samples.push_back(sample);
    report.samples.emplace_back(); // Explicit value initialization avoids GCC optional-copy diagnostics.
    std::ostringstream output;
    output.imbue(std::locale{std::locale::classic(), new CommaPunctuation});
    fluxcrawl::write_benchmark_csv(output, report);
    const std::string expected_header = "version,build_type,seed,workload_pages,fanout,delay_ms,body_bytes,max_depth,max_pages,expected_pages,warmups,engine,configured_concurrency,run,pages_attempted,responses_received,fetch_failures,bytes_downloaded,elapsed_ms,attempts_per_second,responses_per_second,bytes_per_second,latency_samples,p50_ms,p95_ms,p99_ms,server_requests,server_peak_active,scenario,latency_mean_ms,latency_min_ms,latency_max_ms,measured_runs,started_at_unix_ms,shared_anchors,delay_pattern,profile_enabled,extraction_ms,discovery_ms,frontier_ms,extractions,links_considered,links_accepted,lock_wait_ms,worker_idle_ms,worker_activity_ms,lock_acquisitions,cv_waits,worker_wakeups,worker_requests,max_active_workers,multi_wait_ms,completion_processing_ms,refill_ms,event_loop_iterations,multi_waits,completions,refills,max_active_transfers,worker_profiles\n";
    const std::string prefix = "\"v\"\"1\",\"Test\",\"http://127.0.0.1/page/0\",10,2,5,100,20,100,10,0,sequential,1,1,";
    check(output.str() == expected_header + prefix + "100,80,20,1000,2000.000000,50.000000,40.000000,500.000000,100,10.000000,20.000000,30.000000,100,1,\"custom\",15.500000,1.000000,40.000000,0,0,0,\"uniform\",0" + std::string(23, ',') + "\n"
          + prefix + "0,0,0,0,0.000000,,,,0,,,,0,0,\"custom\",,,,0,0,0,\"uniform\",0" + std::string(23, ',') + "\n", "CSV header, columns, quoting, locale, empty fields, rows, newlines");
    sample.elapsed = 4s; sample.metrics.attempts_per_second = 25;
    report.samples[1] = sample;
    auto aggregate = fluxcrawl::aggregate_samples(report.samples);
    check(aggregate.size() == 1 && aggregate[0].median_elapsed_ms == 3000
          && aggregate[0].median_attempts_per_second == 37.5 && aggregate[0].elapsed_stddev_ms == 1000, "even sample median");
    report.samples.push_back(sample);
    aggregate = fluxcrawl::aggregate_samples(report.samples);
    check(aggregate[0].median_elapsed_ms == 4000 && aggregate[0].samples == 3, "odd sample median");
    check(fluxcrawl::aggregate_samples({}).empty(), "empty aggregates");
    if (argc == 2) {
        fluxcrawl::BenchmarkConfig config{fluxcrawl::Url::parse(argv[1]).value()};
        config.runs = 1; config.concurrency = {2};
        auto actual = fluxcrawl::BenchmarkRunner{}.run(config);
        check(actual.samples.size() == 3 && actual.expected_pages == 10, "three engine samples");
        for (const auto& row : actual.samples) {
            check(row.pages_attempted == 10 && row.responses_received == 10 && row.bytes_downloaded == 10240
                  && row.metrics.latency.samples == 10 && row.metrics.latency.p50.has_value()
                  && row.server_requests == 10 && row.server_peak_active <= row.concurrency, "complete measured workload");
        }
        check(actual.samples[0].engine == fluxcrawl::EngineType::Sequential && actual.samples[0].concurrency == 1
              && actual.samples[1].engine == fluxcrawl::EngineType::Threaded
              && actual.samples[2].engine == fluxcrawl::EngineType::Async, "stable engine ordering");
        config.engines = {fluxcrawl::EngineType::Async, fluxcrawl::EngineType::Threaded};
        config.concurrency = {4, 2, 4}; config.runs = 3; config.warmups = 1;
        actual = fluxcrawl::BenchmarkRunner{}.run(config);
        check(actual.samples.size() == 12 && actual.aggregates.size() == 4, "deduplicated 2x2x3 sweep with warmups excluded");
        check(actual.samples[0].engine == fluxcrawl::EngineType::Threaded && actual.samples[0].concurrency == 2
              && actual.samples[4].run == 2 && actual.samples.back().run == 3, "repetition-major increasing concurrency");
        config.warmups = 0; config.runs = 1;
        config.workers = {1, 2}; config.concurrency = {1, 2, 4};
        actual = fluxcrawl::BenchmarkRunner{}.run(config);
        check(actual.samples.size() == 5 && actual.samples[1].concurrency == 2
              && actual.samples.back().concurrency == 4, "independent worker and async sweep sizes");
        config.workers.clear(); config.concurrency = {2, 4};
        config.crawl.max_pages = 0;
        actual = fluxcrawl::BenchmarkRunner{}.run(config);
        for (const auto& row : actual.samples) {
            check(row.pages_attempted == 0 && !row.metrics.latency.p50 && row.server_requests == 0, "zero workload metrics");
        }
        config.crawl = {1, 100, true};
        actual = fluxcrawl::BenchmarkRunner{}.run(config);
        check(actual.expected_pages == 5 && actual.samples[0].pages_attempted == 5, "known reachable depth subset");
        config.crawl.backpressure = {2, 1, true};
        actual = fluxcrawl::BenchmarkRunner{}.run(config);
        for (const auto& row : actual.samples) {
            check(row.pages_attempted == 5 && row.pressure.peak_unfinished <= 2 && row.pressure.activations > 0,
                  "enabled benchmark keeps workload and records hard capacity");
        }
        std::ostringstream pressure_csv; fluxcrawl::write_benchmark_csv(pressure_csv, actual);
        check(pressure_csv.str().find("pending_high,pending_low,peak_pending,pressure_activations,pressure_ms") != std::string::npos,
              "enabled benchmark exports reproducible pressure metadata");
        const auto rejects = [&](fluxcrawl::BenchmarkConfig bad) {
            try { (void)fluxcrawl::BenchmarkRunner{}.run(bad); }
            catch (const std::exception&) { return true; }
            return false;
        };
        auto bad = config; bad.runs = 0; check(rejects(bad), "zero runs");
        bad = config; bad.engines.clear(); check(rejects(bad), "empty engine set");
        bad = config; bad.concurrency = {0}; check(rejects(bad), "zero concurrency");
        bad = config; bad.concurrency.clear(); check(rejects(bad), "empty concurrency");
        bad = config; bad.crawl.max_pages = 2; check(rejects(bad), "truncating budget rejected");
        bad = config; bad.seed = fluxcrawl::Url::parse("https://example.com/page/0").value();
        check(rejects(bad), "public benchmark seed rejected before fetch");
    }
    std::cout << failures << " benchmark failures\n";
    return failures ? 1 : 0;
}
