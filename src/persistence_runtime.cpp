#include "persistence_runtime.hpp"
#include <fluxcrawl/sqlite_store.hpp>
#include <iostream>

namespace fluxcrawl::detail {
PersistenceRuntime::PersistenceRuntime(const Url& seed, const CrawlOptions& options, const HttpClientOptions& http)
    : seed_{seed}, options_{options}, http_{http}, snapshots_{seed, options, http},
      mode_{!options.sqlite.path.empty() ? PersistenceMode::Sqlite : checkpointing(options) ? PersistenceMode::Snapshot : PersistenceMode::None},
      sequence_{options.resume_from ? options.resume_from->sequence : 0}
{
    options.dedup.validate();
    if (options.dedup.mode != DedupMode::Memory && (mode_ != PersistenceMode::Sqlite || checkpointing(options))) {
        throw std::invalid_argument("SQLite dedup requires --state-db and cannot use snapshots");
    }
    if (mode_ == PersistenceMode::Sqlite) {
        if (checkpointing(options)) { throw std::invalid_argument("Choose one persistence backend: snapshot or SQLite"); }
        if (options.sqlite.batch_size == 0) { throw std::invalid_argument("SQLite batch size must be positive"); }
        if (options.sqlite.resume != bool(options.resume_from)) { throw std::invalid_argument("SQLite resume requires an existing database restore model"); }
    }
}
void PersistenceRuntime::initialize(CrawlState& state, HostScheduler& scheduler, CrawlPolicy& policy, CrawlResult& result)
{
    const auto restore_began=options_.resume_from && options_.resume_from->restore_started_at ? *options_.resume_from->restore_started_at : HostScheduler::Clock::now();
    auto prefix = mode_ == PersistenceMode::Sqlite ? options_.sqlite.path : options_.checkpoint.path;
    if (mode_ == PersistenceMode::Snapshot && !prefix.empty() && !std::filesystem::exists(std::filesystem::absolute(prefix).parent_path())) { prefix.clear(); }
    state.configure_spool(prefix, mode_ != PersistenceMode::None);
    if (mode_ != PersistenceMode::Sqlite) { restore_session(seed_, options_, http_, state, scheduler, policy, result); return; }
    if (options_.sqlite.resume) {
        if (options_.resume_from->opened_store) { store_=options_.resume_from->opened_store; }
        else {
            auto opened=SqliteStore::open(options_.sqlite.path); const auto loaded=opened->load();
            if(loaded.sequence!=options_.resume_from->sequence || loaded.seed!=seed_) { throw std::runtime_error("Database changed since resume model was loaded"); }
            store_=std::move(opened);
        }
    } else {
        CrawlCheckpoint initial{.seed = seed_}; initial.options = options_; initial.http = http_;
        initial.seen = {seed_}; initial.unfinished = {{{seed_, 0}, {}, false}};
        initial.counters.urls_discovered = initial.counters.urls_pending = 1;
        store_ = SqliteStore::create(options_.sqlite.path, initial);
    }
    if(options_.dedup.mode!=DedupMode::Memory) { state.configure_seen(sqlite_seen(store_,options_.dedup)); }
    state.set_mutation_sink(&journal_); scheduler.set_mutation_sink(&journal_);
    restore_session(seed_, options_, http_, state, scheduler, policy, result);
    // Rewrite unfinished states once on startup: former active rows become ready,
    // and held-policy resources retain their retry deadlines. No seen re-inserts.
    auto pending = scheduler.snapshot(); const auto held = policy.snapshot(); pending.insert(pending.end(), held.begin(), held.end());
    for (const auto& work : pending) { journal_.work(work.item, work.eligible_at, false); }
    if(options_.resume_from) { const auto timings=store_->load_timings(); result.restore_open_time=timings.open; result.restore_validation_time=timings.seen+timings.work+timings.validation; result.restore_time=HostScheduler::Clock::now()-restore_began; }
    result.persistence.initialization_time = store_->metrics().initialization_time;
}
std::optional<PersistenceRuntime::Request> PersistenceRuntime::capture(CrawlState& state, const HostScheduler& scheduler,
                                                                      const CrawlPolicy& policy, const CrawlResult& result, bool final)
{
    if (mode_ != PersistenceMode::Sqlite) {
        auto snapshot = snapshots_.capture(state, scheduler, policy, result, final);
        if (!snapshot) { return {}; }
        return Request{std::move(snapshot), false, final};
    }
    if (failed_.load() || (!final && journal_.operations() < options_.sqlite.batch_size)) { return {}; }
    if (sequence_ >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1) { throw std::overflow_error("SQLite transaction sequence exhausted"); }
    CrawlCheckpoint source{.seed = seed_}; source.options = options_; source.http = http_; source.sequence = sequence_ + 1;
    auto metadata = CheckpointManager::metadata_only(source); metadata.counters = CheckpointManager::copy_counters(result);
    auto& counters = metadata.counters; counters.urls_discovered = state.seen_count();
    counters.urls_pending = state.seen_count() - counters.resources_completed - counters.urls_blocked_by_robots - counters.robots_resources_reused;
    counters.retries_cancelled = counters.retries_scheduled + counters.recovery_retry_attempts - counters.retries_attempted;
    counters.robots_retries_cancelled = counters.robots_retries_scheduled - counters.robots_retries_attempted;
    metadata.deferred = state.spool() ? state.spool()->capture() : DeferredState{}; metadata.spool_sync = state.spool();
    metadata.completed = counters.urls_pending == 0 && metadata.deferred.count() == 0;
    if (journal_.active() > scheduler.active()) { throw std::logic_error("Persistence active-work accounting disagrees"); }
    auto batch = std::make_shared<CrawlMutationBatch>(journal_.batch(std::move(metadata), scheduler.active() - journal_.active()));
    std::size_t queued = 0;
    { std::lock_guard lock{queue_mutex_}; queue_.push_back(batch); queued = queue_.size(); }
    state.captured(sequence_ + 1, batch->discoveries);
    ++sequence_; journal_.clear(); // Only detach after the ordered queue owns the complete batch.
    return Request{{}, true, final, batch->operations, queued};
}
PersistenceRuntime::Report PersistenceRuntime::write(const Request& request)
{
    if (!request.database) { return {snapshots_.write(*request.snapshot), {}, {}, false}; }
    Report report; report.is_database = true;
    report.database.buffered_mutations_peak = request.buffered; report.database.queued_batches_peak = request.queued;
    std::lock_guard writer{writer_mutex_};
    if (failed_.load()) { report.error = "SQLite persistence previously failed; durable prefix preserved"; return report; }
    const auto before = store_->metrics();
    try {
        for (;;) {
            std::shared_ptr<const CrawlMutationBatch> batch;
            { std::lock_guard lock{queue_mutex_}; if (queue_.empty()) { break; } batch = queue_.front(); }
            store_->apply(*batch); // Neither crawl mutex nor queue mutex is held during I/O.
            { std::lock_guard lock{queue_mutex_}; queue_.pop_front(); }
        }
    } catch (const std::exception& error) {
        failed_.store(true); report.error = error.what(); std::cerr << "SQLite persistence failed: " << error.what() << '\n';
        std::lock_guard lock{queue_mutex_}; queue_.clear();
    }
    const auto after = store_->metrics();
    report.database.seen_insertions = after.seen_insertions - before.seen_insertions;
    report.database.seen_constraint_conflicts = after.seen_constraint_conflicts - before.seen_constraint_conflicts;
    report.database.transactions = after.transactions - before.transactions;
    report.database.mutations = after.mutations - before.mutations;
    report.database.row_changes = after.row_changes - before.row_changes;
    report.database.commit_time = after.commit_time - before.commit_time;
    report.database.max_commit_time = after.max_commit_time;
    report.database.last_commit_time = after.last_commit_time;
    if (request.final) { report.database.file_bytes = after.file_bytes; }
    return report;
}
void PersistenceRuntime::report(CrawlResult& result, Report report)
{
    if (!report.is_database) { CheckpointRuntime::report(result, std::move(report.snapshot)); return; }
    if (!report.error.empty()) { result.persistence_errors.push_back(std::move(report.error)); }
    result.persistence.buffered_mutations_peak = std::max(result.persistence.buffered_mutations_peak, report.database.buffered_mutations_peak);
    result.persistence.queued_batches_peak = std::max(result.persistence.queued_batches_peak, report.database.queued_batches_peak);
    result.persistence.seen_insertions += report.database.seen_insertions; result.persistence.seen_constraint_conflicts += report.database.seen_constraint_conflicts;
    result.persistence.transactions += report.database.transactions; result.persistence.mutations += report.database.mutations;
    if (report.database.transactions) { result.persistence.last_commit_time = report.database.last_commit_time; }
    result.persistence.row_changes += report.database.row_changes; result.persistence.commit_time += report.database.commit_time;
    result.persistence.max_commit_time = std::max(result.persistence.max_commit_time, report.database.max_commit_time);
    if (report.database.file_bytes) { result.persistence.file_bytes = report.database.file_bytes; }
}
} // namespace fluxcrawl::detail
