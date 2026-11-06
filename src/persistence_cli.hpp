#pragma once
#include <fluxcrawl/crawl_types.hpp>
#include <ostream>
namespace fluxcrawl::detail {
inline void print_dedup_metrics(std::ostream& out,const CrawlResult& result,const DedupOptions& options)
{
    const auto& d=result.dedup;
    out<<"Dedup backend: "<<dedup_name(options.mode)<<"\nDedup membership queries: "<<d.queries
       <<"\nDedup lookup time ms: "<<std::chrono::duration<double,std::milli>(d.lookup_time).count()
       <<"\nDedup insert time ms: "<<std::chrono::duration<double,std::milli>(d.insert_time).count()
       <<"\nExact DB checks: "<<d.exact_db_queries<<"\nConfirmed duplicates: "<<d.confirmed_duplicates
       <<"\nUncommitted seen cache peak: "<<d.pending_cache_peak<<"\nUncommitted seen cache remaining: "<<d.pending_cache_size<<'\n';
    if(options.mode==DedupMode::BloomSqlite) {
        out<<"Bloom expected elements: "<<options.expected_elements<<"\nBloom target FP rate: "<<options.false_positive_rate
           <<"\nBloom bytes: "<<d.bloom_bytes<<"\nBloom bits: "<<d.bloom_bits<<"\nBloom probes: "<<d.bloom_probes
           <<"\nBloom insertions: "<<d.bloom_insertions<<"\nBloom occupancy: "<<(d.bloom_bits?double(d.bloom_set_bits)/double(d.bloom_bits):0)
           <<"\nBloom queries: "<<d.bloom_queries<<"\nBloom definite misses: "<<d.bloom_definite_misses
           <<"\nBloom maybe hits: "<<d.bloom_maybe_hits<<"\nExact checks after Bloom: "<<d.exact_checks_after_bloom
           <<"\nBloom false positives: "<<d.bloom_false_positives
           <<"\nBloom rebuild time ms: "<<std::chrono::duration<double,std::milli>(d.rebuild_time).count()
           <<"\nBloom capacity exceeded: "<<(d.bloom_insertions>options.expected_elements?"yes":"no")<<'\n';
    }
}
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
