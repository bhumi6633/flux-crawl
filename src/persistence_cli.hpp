#pragma once
#include <fluxcrawl/crawl_types.hpp>
#include <ostream>
namespace fluxcrawl::detail {
inline void print_pressure_metrics(std::ostream& out, const CrawlResult& result, const BackpressureOptions& options)
{
    const auto& p = result.pressure;
    out << "Pending high watermark: " << (options.enabled ? options.high_watermark : 0)
        << "\nPending low watermark: " << (options.enabled ? options.low_watermark : 0)
        << "\nPeak pending URLs: " << p.peak_pending << "\nPeak admitted unfinished: " << p.peak_unfinished
        << "\nPeak seen URLs: " << p.peak_seen << "\nDeferred discovery URLs: " << p.deferred_urls
        << "\nPeak deferred discoveries: " << p.peak_deferred << "\nDeferred admissions: " << p.deferred_admissions
        << "\nBackpressure activations: " << p.activations << "\nAdmission resumptions: " << p.resumptions
        << "\nDeferred spool I/O time ms: " << std::chrono::duration<double, std::milli>(p.deferred_io_time).count()
        << "\nTime under pressure ms: " << std::chrono::duration<double, std::milli>(p.pressure_time).count() << '\n';
}
inline void print_database_metrics(std::ostream& out, const CrawlResult& result)
{
    const auto& m = result.persistence;
    out << "Persistence mode: sqlite\nSQLite transactions: " << m.transactions
        << "\nSQLite peak buffered mutations: " << m.buffered_mutations_peak
        << "\nSQLite peak queued batches: " << m.queued_batches_peak
        << "\nSQLite mutations: " << m.mutations << "\nSQLite row changes: " << m.row_changes
        << "\nSQLite average batch mutations: " << (m.transactions ? static_cast<double>(m.mutations) / static_cast<double>(m.transactions) : 0)
        << "\nSQLite commit time ms: " << std::chrono::duration<double, std::milli>(m.commit_time).count()
        << "\nSQLite max commit ms: " << std::chrono::duration<double, std::milli>(m.max_commit_time).count()
        << "\nSQLite initialization ms: " << std::chrono::duration<double, std::milli>(m.initialization_time).count()
        << "\nSQLite bytes (main + WAL): " << m.file_bytes
        << "\nSQLite persistence errors: " << result.persistence_errors.size() << '\n';
}
} // namespace fluxcrawl::detail
