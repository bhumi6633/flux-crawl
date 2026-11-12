#include <fluxcrawl/observability.hpp>
#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/crawl_policy.hpp>
#include <fluxcrawl/checkpoint.hpp>
#include <algorithm>
#include <stdexcept>

namespace fluxcrawl {
Observability::Observability(std::chrono::milliseconds interval, bool topology)
    : interval_{interval}, topology_{topology}
{
    if (interval < std::chrono::milliseconds{100} || interval > std::chrono::seconds{10}) {
        throw std::invalid_argument("metrics interval must be 100..10000 ms");
    }
    std::atomic_store(&latest_,std::make_shared<const ObservabilitySnapshot>());
}
void Observability::begin(std::string engine, std::size_t concurrency)
{
    engine_ = std::move(engine); concurrency_ = concurrency;
    started_ = Clock::now(); next_ = started_; recent_.clear(); throughput_.clear();
    nodes_.clear(); edges_.clear(); errors_.clear(); max_active_=worker_waits_=iterations_=completions_=0;
    last_exhausted_=successes_=transfers_finished_=0; lock_wait_={}; loop_wait_={};
}
void Observability::error(std::string message) noexcept try
{
    errors_.push_back(message.substr(0, 160));
    if (errors_.size() > 16) { errors_.pop_front(); }
} catch (...) {}
void Observability::failed() {
    auto out=std::make_shared<ObservabilitySnapshot>(*latest());
    out->state="error"; out->active=0; ++out->sequence;
    out->errors.push_back("Crawler stopped after an exception; counters reflect last safe sample");
    out->elapsed_ms=std::chrono::duration<double,std::milli>(Clock::now()-started_).count();
    std::atomic_store(&latest_,std::shared_ptr<const ObservabilitySnapshot>{std::move(out)});
}
void Observability::request_finished(const CrawlRecord& record) noexcept try
{
    ++transfers_finished_;
    if (record.item.policy_request) { return; }
    ++completions_;
    const auto* response=std::get_if<CrawlResponseMetadata>(&record.outcome);
    if (response && response->status_code>=200 && response->status_code<300) { ++successes_; }
    const auto now = Clock::now();
    recent_.push_back({now, record.latency});
    while (recent_.size() > 4096 || (!recent_.empty() && now-recent_.front().time > std::chrono::seconds{5})) { recent_.pop_front(); }
    const auto bucket = Clock::time_point{std::chrono::duration_cast<Clock::duration>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) / 100 * 100)};
    if (throughput_.empty() || throughput_.back().first != bucket) { throughput_.emplace_back(bucket, 0); }
    ++throughput_.back().second;
    while (throughput_.size()>51 || (!throughput_.empty() && now-throughput_.front().first >= std::chrono::seconds{5})) { throughput_.pop_front(); }
    if (std::holds_alternative<HttpError>(record.outcome)) { error("Transport/fetch failure at " + record.item.url.host()); }
    else if (std::get<CrawlResponseMetadata>(record.outcome).status_code >= 500) { error("HTTP 5xx at " + record.item.url.host()); }
} catch (...) {}
void Observability::discovery(const Url& parent, const Url& child) noexcept try
{
    if (!topology_) { return; }
    const auto index = [&](const Url& url) -> std::optional<std::size_t> {
        const auto key = HostKey::from(url);
        const auto host = key.scheme + "://" + key.host + ":" + std::to_string(key.effective_port);
        auto found = std::find(nodes_.begin(), nodes_.end(), host);
        if (found != nodes_.end()) { return static_cast<std::size_t>(found - nodes_.begin()); }
        if (nodes_.size() == 64) { return {}; }
        nodes_.push_back(host); return nodes_.size()-1;
    };
    const auto a=index(parent), b=index(child);
    if (a && b && edges_.size()<128) { edges_.emplace(*a,*b); }
} catch (...) {}
void Observability::capture(const CrawlState& state, HostScheduler& scheduler, const CrawlPolicy& policy,
                            const CrawlResult& result, const CrawlOptions& options, bool final, std::size_t pending_mutations) noexcept try
{
    const auto now=Clock::now(); max_active_=std::max(max_active_, scheduler.active());
    if (!final && now<next_) { return; }
    next_=now+interval_;
    auto out=std::make_shared<ObservabilitySnapshot>();
    out->successes=successes_; out->waiting_hosts=policy.waiting_hosts();
    out->sequence=++sequence_; out->engine=engine_; out->concurrency=concurrency_;
    out->active=scheduler.active(); out->max_active=max_active_; out->delayed=scheduler.delayed(); out->held=policy.held();
    out->elapsed_ms=std::chrono::duration<double,std::milli>(now-started_).count();
    out->counters=CheckpointManager::copy_counters(result);
    out->counters.urls_discovered=state.seen_count();
    out->counters.urls_pending=scheduler.resource_pending()+out->held+state.frontier_size();
    out->counters.pressure=state.pressure_metrics(); out->counters.dedup=state.dedup_metrics();
    out->counters.persistence=result.persistence;
    out->counters.checkpoints_written=result.checkpoints_written; out->counters.checkpoint_bytes=result.checkpoint_bytes;
    out->counters.checkpoint_time=result.checkpoint_time;
    out->state=final ? (out->counters.urls_pending || out->counters.pressure.deferred_urls ? "stopped" : "completed") : "running";
    if (!result.persistence_errors.empty() || !result.checkpoint_errors.empty()) { out->state="error"; }
    out->average_rps=out->elapsed_ms>0 ? static_cast<double>(completions_)*1000/out->elapsed_ms : 0;
    std::size_t count=0;
    while (!throughput_.empty() && now-throughput_.front().first>=std::chrono::seconds{5}) { throughput_.pop_front(); }
    for (const auto& bucket : throughput_) { count+=bucket.second; }
    out->rolling_rps=static_cast<double>(count)/std::max(0.001,std::min(5.0,out->elapsed_ms/1000));
    while (!recent_.empty() && now-recent_.front().time>std::chrono::seconds{5}) { recent_.pop_front(); }
    std::vector<std::chrono::nanoseconds> latencies; latencies.reserve(recent_.size());
    for (const auto& sample:recent_) { latencies.push_back(sample.latency); }
    out->latency=latency_stats(latencies);
    out->hosts=scheduler.sample(now); out->pressure_options=options.backpressure; out->dedup_options=options.dedup;
    out->scheduling=options.scheduling;
    out->persistence_mode=!options.sqlite.path.empty()?"sqlite":!options.checkpoint.path.empty()?"snapshot":"off";
    out->pending_mutations=pending_mutations; out->last_commit_ms=std::chrono::duration<double,std::milli>(result.persistence.last_commit_time).count();
    out->last_checkpoint_ms=std::chrono::duration<double,std::milli>(result.last_checkpoint_duration).count();
    out->counters.last_checkpoint_unix_ms=result.last_checkpoint_unix_ms;
    out->worker_waits=worker_waits_; out->lock_wait_ms=std::chrono::duration<double,std::milli>(lock_wait_).count();
    out->iterations=iterations_; out->completions=transfers_finished_; out->loop_wait_ms=std::chrono::duration<double,std::milli>(loop_wait_).count();
    out->topology_enabled=topology_; out->topology_hosts=nodes_; out->topology_edges.assign(edges_.begin(),edges_.end());
    if (result.retries_exhausted>last_exhausted_) { error("Retry exhaustion: " + std::to_string(result.retries_exhausted) + " resources total"); last_exhausted_=result.retries_exhausted; }
    out->errors.assign(errors_.begin(),errors_.end());
    for (const auto* errors : {&result.persistence_errors,&result.checkpoint_errors}) {
        for (auto i=errors->size()>4?errors->size()-4:0; i<errors->size(); ++i) { out->errors.push_back((*errors)[i].substr(0,160)); }
    }
    std::atomic_store(&latest_,std::shared_ptr<const ObservabilitySnapshot>{std::move(out)});
} catch (...) { /* Best-effort telemetry must not fail a crawl. */ }
}
