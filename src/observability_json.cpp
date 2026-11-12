#include <fluxcrawl/observability_server.hpp>
#include <boost/json.hpp>
namespace fluxcrawl {
namespace j=boost::json;
namespace {
double ms(std::chrono::nanoseconds n) { return std::chrono::duration<double,std::milli>(n).count(); }
j::value percentile(const std::optional<std::chrono::nanoseconds>& n) { return n ? j::value{ms(*n)} : j::value{}; }
}
std::string observability_json(const ObservabilitySnapshot& s)
{
    const auto& r=s.counters; const auto& d=r.dedup; const auto& p=r.persistence;
    j::object engine{{"name",s.engine},{"configured_concurrency",s.concurrency},{"max_active",s.max_active}};
    if (s.engine=="threaded") {
        engine["workers"]=s.concurrency; engine["active_workers"]=s.active;
        engine["idle_workers"]=s.concurrency-s.active; engine["worker_waits"]=s.worker_waits; engine["lock_wait_ms"]=s.lock_wait_ms;
    } else if (s.engine=="async") {
        engine["active_transfers"]=s.active; engine["iterations"]=s.iterations;
        engine["wait_ms"]=s.loop_wait_ms; engine["completions"]=s.completions;
    }
    j::array hosts;
    for (const auto& h:s.hosts.hosts) {
        hosts.emplace_back(j::object{{"origin",h.origin},{"pending",h.pending},{"active",h.active},{"delayed",h.delayed},{"completed",h.completed},
            {"per_host_limit",s.scheduling.max_per_host.value_or(s.concurrency)},
            {"rate_rps",s.scheduling.rate?j::value{s.scheduling.rate->requests_per_second}:j::value{}},
            {"burst",s.scheduling.rate?j::value{s.scheduling.rate->burst}:j::value{}},{"wait_ms",h.wait_ms}});
    }
    j::object dedup{{"backend",dedup_name(s.dedup_options.mode)},{"seen",r.urls_discovered},{"duplicates",d.confirmed_duplicates},
        {"lookup_ms",ms(d.lookup_time)},{"exact_db_queries",d.exact_db_queries}};
    if (s.dedup_options.mode==DedupMode::BloomSqlite) {
        dedup["bloom"]=j::object{{"bytes",d.bloom_bytes},{"occupancy",d.bloom_bits?static_cast<double>(d.bloom_set_bits)/static_cast<double>(d.bloom_bits):0},
            {"target_fp",s.dedup_options.false_positive_rate},{"observed_fp",(d.bloom_definite_misses+d.bloom_false_positives)?j::value{static_cast<double>(d.bloom_false_positives)/static_cast<double>(d.bloom_definite_misses+d.bloom_false_positives)}:j::value{}},
            {"inserted",d.bloom_insertions},{"expected",s.dedup_options.expected_elements},{"definite_negatives",d.bloom_definite_misses},
            {"maybe_hits",d.bloom_maybe_hits},{"false_positives",d.bloom_false_positives},{"exact_checks",d.exact_checks_after_bloom}};
    }
    j::array nodes,edges,errors;
    for (std::size_t i=0;i<s.topology_hosts.size();++i) { nodes.emplace_back(j::object{{"id",i},{"host",s.topology_hosts[i]}}); }
    for (auto [a,b]:s.topology_edges) { edges.emplace_back(j::object{{"source",a},{"target",b}}); }
    for (const auto& e:s.errors) { errors.emplace_back(e); }
    j::object data{
        {"schema_version",1},{"sequence",s.sequence},{"state",s.state},{"running",s.state=="running"},{"elapsed_ms",s.elapsed_ms},{"engine",engine},
        {"progress",j::object{{"seen",r.urls_discovered},{"pending",r.urls_pending},{"active",s.active},{"completed",r.resources_completed},
            {"blocked",r.urls_blocked_by_robots},{"attempts",r.pages_attempted},{"responses",r.responses_received},{"successful_responses_session",s.successes},{"fetch_failures",r.fetch_failures}}},
        {"throughput",j::object{{"rolling_rps",s.rolling_rps},{"average_rps",s.average_rps},{"window_ms",5000},{"basis","resource attempt completions, current session"}}},
        {"latency",j::object{{"scope","recent 5s, last 4096 resource attempts"},{"samples",s.latency.samples},{"p50_ms",percentile(s.latency.p50)},
            {"p95_ms",percentile(s.latency.p95)},{"p99_ms",percentile(s.latency.p99)},{"mean_ms",s.latency.mean_ms?j::value{*s.latency.mean_ms}:j::value{}}}},
        {"queues",j::object{{"pending",r.urls_pending},{"delayed",s.delayed},{"robots_held",s.held},{"deferred",r.pressure.deferred_urls}}},
        {"retry",j::object{{"delayed",s.delayed},{"scheduled",r.retries_scheduled},{"attempted",r.retries_attempted},{"exhausted",r.retries_exhausted},
            {"recovered",r.responses_recovered_after_retry},{"next_eligible_ms",s.hosts.next_retry_ms?j::value{*s.hosts.next_retry_ms}:j::value{}}}},
        {"robots",j::object{{"fetches",r.robots_fetches},{"cache_hits",r.robots_cache_hits},{"blocked",r.urls_blocked_by_robots},{"failures",r.robots_fetch_failures},{"held_urls",s.held},{"waiting_hosts",s.waiting_hosts}}},
        {"backpressure",j::object{{"enabled",s.pressure_options.enabled},{"active",r.pressure.activations>r.pressure.resumptions},
            {"pending",r.pressure.current_pending},{"high",s.pressure_options.high_watermark},{"low",s.pressure_options.low_watermark},
            {"peak_pending",r.pressure.peak_pending},{"activations",r.pressure.activations},{"time_ms",ms(r.pressure.pressure_time)}}},
        {"dedup",dedup},
        {"persistence",j::object{{"mode",s.persistence_mode},{"transactions",p.transactions},{"pending_mutations",s.pending_mutations},
            {"average_batch",p.transactions?static_cast<double>(p.mutations)/static_cast<double>(p.transactions):0},
            {"commit_ms",ms(p.commit_time)},{"last_commit_ms",s.last_commit_ms},{"database_bytes",p.file_bytes},
            {"checkpoints",r.checkpoints_written},{"checkpoint_bytes",r.checkpoint_bytes},{"checkpoint_ms",ms(r.checkpoint_time)},{"last_checkpoint_ms",s.last_checkpoint_ms},{"last_checkpoint_unix_ms",r.last_checkpoint_unix_ms}}},
        {"hosts",j::object{{"total_hosts",s.hosts.total_hosts},{"candidates",s.hosts.candidates},{"selection","top 32 by pending+active from at most 256 candidate hosts"},{"items",hosts}}},
        {"topology",j::object{{"enabled",s.topology_enabled},{"kind","first discovered origin graph"},{"max_nodes",64},{"max_edges",128},{"nodes",nodes},{"edges",edges}}},
        {"errors",errors}
    };
    return j::serialize(data);
}
}
