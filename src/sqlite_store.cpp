#include <fluxcrawl/sqlite_store.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fluxcrawl {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int application_id = 0x464C5853;
[[noreturn]] void failure(sqlite3* db, int code, const char* operation)
{
    throw std::runtime_error(std::string{"SQLite "} + operation + " [" + std::to_string(code) + "/"
        + std::to_string(db ? sqlite3_extended_errcode(db) : code) + "]: " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(code)));
}
void check(sqlite3* db, int code, const char* operation) { if (code != SQLITE_OK) { failure(db, code, operation); } }
void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(std::string{"Invalid crawl database: "} + message); } }
void exec(sqlite3* db, const char* sql) { check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr), "statement"); }
struct Ownership {
#if defined(__unix__) || defined(__APPLE__)
    int fd = -1;
    explicit Ownership(const std::filesystem::path& path) {
        auto lock_path = std::filesystem::canonical(path); lock_path += ".owner";
        // Keep application ownership separate from SQLite's own file locks.
        fd = ::open(lock_path.string().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) { throw std::runtime_error("Cannot open crawl DB ownership handle: " + std::string{std::strerror(errno)}); }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); fd = -1; throw std::runtime_error("Crawl database already owned or cannot be locked"); }
    }
    ~Ownership() { if (fd >= 0) { ::close(fd); } }
#else
    explicit Ownership(const std::filesystem::path&) {}
#endif
};
struct Connection {
    sqlite3* db = nullptr;
    explicit Connection(const std::filesystem::path& path) {
        const auto code = sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (code != SQLITE_OK) { const auto error = db ? std::string{sqlite3_errmsg(db)} : std::string{sqlite3_errstr(code)}; if (db) { sqlite3_close_v2(db); db = nullptr; } throw std::runtime_error("SQLite open [" + std::to_string(code) + "]: " + error); }
        sqlite3_extended_result_codes(db, 1);
        sqlite3_busy_timeout(db, 1000);
    }
    ~Connection() { if (db) { sqlite3_close_v2(db); } }
};
struct Statement {
    sqlite3* db;
    sqlite3_stmt* statement = nullptr;
    Statement(sqlite3* database, const char* sql) : db{database} { check(db, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr), "prepare"); }
    ~Statement() { if (statement) { sqlite3_finalize(statement); } }
    void start() { (void)sqlite3_reset(statement); check(db, sqlite3_clear_bindings(statement), "clear bindings"); } // reset repeats a previously reported step error.
    void text(int index, std::string_view value) { check(db, sqlite3_bind_text64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind text"); }
    void number(int index, std::uint64_t value) { require(value <= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()), "integer exceeds SQLite signed range"); check(db, sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)), "bind integer"); }
    void null(int index) { check(db, sqlite3_bind_null(statement, index), "bind null"); }
    bool row() { const auto code = sqlite3_step(statement); if (code == SQLITE_ROW) { return true; } if (code == SQLITE_DONE) { return false; } failure(db, code, "step"); }
    void done() { const auto code = sqlite3_step(statement); if (code != SQLITE_DONE) { failure(db, code, "step"); } }
    std::string string(int column) const {
        require(sqlite3_column_type(statement, column) == SQLITE_TEXT, "expected text column");
        const auto* value = sqlite3_column_text(statement, column); const auto length = sqlite3_column_bytes(statement, column);
        require(value != nullptr, "unreadable text column"); return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(length)};
    }
    std::uint64_t integer(int column) const { require(sqlite3_column_type(statement, column) == SQLITE_INTEGER, "expected integer column"); const auto value = sqlite3_column_int64(statement, column); require(value >= 0, "negative integer column"); return static_cast<std::uint64_t>(value); }
};
struct Transaction {
    sqlite3* db; bool committed = false;
    explicit Transaction(sqlite3* value) : db{value} { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) { (void)sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); } }
    void commit() { exec(db, "COMMIT"); committed = true; }
};
std::uint64_t scalar(sqlite3* db, const char* sql) { Statement s{db, sql}; require(s.row(), "missing scalar"); return s.integer(0); }
Url canonical(const std::string& text) { const auto url = Url::parse(text); require(url && url->str() == text, "noncanonical/invalid URL"); return *url; }
std::size_t checked_size(std::uint64_t value) { require(value <= std::numeric_limits<std::size_t>::max(), "size overflow"); return static_cast<std::size_t>(value); }
void increment(std::size_t& value, std::size_t extra) { require(extra <= std::numeric_limits<std::size_t>::max() - value, "counter overflow"); value += extra; }
}
struct SqliteStore::Impl {
    mutable std::mutex mutex;
    std::atomic<std::uint64_t> committed{0};
    std::filesystem::path path;
    Ownership owner;
    Connection connection;
    std::unique_ptr<Statement> insert_seen, upsert_work, delete_work, select_work, set_meta, lookup_seen;
    std::optional<CrawlCheckpoint> metadata;
    std::size_t seen_count = 0, work_count = 0, active_count = 0;
    bool validated = false;
    PersistenceMetrics stats;
    StoreLoadTimings timings;
    explicit Impl(const std::filesystem::path& value) : path{value}, owner{value}, connection{value} {}
    sqlite3* db() const { return connection.db; }
    void configure() {
        exec(db(), "PRAGMA foreign_keys=ON");
        { Statement wal{db(), "PRAGMA journal_mode=WAL"}; require(wal.row() && wal.string(0) == "wal", "WAL unavailable"); }
        exec(db(), "PRAGMA synchronous=FULL");
        require(scalar(db(), "PRAGMA synchronous") == 2, "FULL synchronous unavailable");
#if !defined(__unix__) && !defined(__APPLE__)
        exec(db(), "PRAGMA locking_mode=EXCLUSIVE");
#endif
    }
    void prepare() {
        lookup_seen = std::make_unique<Statement>(db(), "SELECT 1 FROM seen_urls WHERE url=?1");
        insert_seen = std::make_unique<Statement>(db(), "INSERT INTO seen_urls(url) VALUES(?1)");
        upsert_work = std::make_unique<Statement>(db(), "INSERT INTO unfinished(url,depth,retry_count,recovery,state,eligible_ms,queue_order) VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(url) DO UPDATE SET depth=excluded.depth,retry_count=excluded.retry_count,recovery=excluded.recovery,state=excluded.state,eligible_ms=excluded.eligible_ms,queue_order=excluded.queue_order");
        delete_work = std::make_unique<Statement>(db(), "DELETE FROM unfinished WHERE url=?1");
        select_work = std::make_unique<Statement>(db(), "SELECT state FROM unfinished WHERE url=?1");
        set_meta = std::make_unique<Statement>(db(), "INSERT INTO crawl_meta(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    }
    void meta(std::string_view key, std::string_view value) { auto& s = *set_meta; s.start(); s.text(1, key); s.text(2, value); s.done(); }
    std::string meta(std::string_view key) {
        Statement s{db(), "SELECT value FROM crawl_meta WHERE key=?1"}; s.text(1, key); require(s.row(), "required metadata missing"); return s.string(0);
    }
    void work(const CheckpointWork& work, std::uint64_t order) {
        const auto& item = work.item;
        require(!item.policy_request, "invalid policy work");
        auto& s = *upsert_work; s.start(); s.text(1, item.url.str()); s.number(2, item.depth); s.number(3, item.retry_count); s.number(4, item.recovery);
        s.number(5, work.was_active ? 2 : work.eligible_unix_ms ? 1 : 0);
        if (work.eligible_unix_ms) { require(*work.eligible_unix_ms >= 0 && !work.was_active && item.retry_count > 0, "invalid retry deadline"); s.number(6, static_cast<std::uint64_t>(*work.eligible_unix_ms)); } else { s.null(6); }
        s.number(7, order); s.done();
    }
    void aggregate(const CrawlCheckpoint& c, std::size_t seen, std::size_t pending, std::size_t active) {
        const auto& r = c.counters;
        require(r.urls_discovered == seen && r.urls_pending == pending && c.completed == (pending == 0 && c.deferred.count() == 0), "row counts/completed marker disagree");
        auto remainder = seen;
        for (auto value : {r.resources_completed, r.urls_blocked_by_robots, r.robots_resources_reused, pending}) { require(value <= remainder, "resource counters disagree"); remainder -= value; }
        require(remainder == 0 && r.pages_attempted <= c.options.max_pages, "resource/budget counters disagree");
        remainder = r.pages_attempted;
        for (auto value : {r.responses_received, r.fetch_failures, r.attempts_unresolved, active}) { require(value <= remainder, "attempt counters disagree"); remainder -= value; }
        require(remainder == 0, "attempt counters incomplete");
    }
};
SqliteStore::SqliteStore(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
SqliteStore::~SqliteStore() = default;
std::string SqliteStore::library_version() { return sqlite3_libversion(); }
std::unique_ptr<SqliteStore> SqliteStore::create(const std::filesystem::path& path, const CrawlCheckpoint& initial)
{
    CheckpointManager::validate(initial, std::numeric_limits<std::size_t>::max());
    auto* file = std::fopen(path.string().c_str(), "wx");
    if (!file) { throw std::runtime_error("Cannot create new crawl database (must not exist): " + std::string{std::strerror(errno)}); }
    if (std::fclose(file) != 0) { throw std::runtime_error("Cannot close new crawl database file"); }
    try {
        const auto began = Clock::now();
        auto impl = std::make_unique<Impl>(path); impl->configure();
        Transaction transaction{impl->db()};
        exec(impl->db(), "PRAGMA application_id=1179408467");
        exec(impl->db(), "PRAGMA user_version=1");
        exec(impl->db(), "CREATE TABLE crawl_meta(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL) WITHOUT ROWID; CREATE TABLE seen_urls(url TEXT PRIMARY KEY NOT NULL) WITHOUT ROWID; CREATE TABLE unfinished(url TEXT PRIMARY KEY NOT NULL REFERENCES seen_urls(url),depth INTEGER NOT NULL CHECK(depth>=0),retry_count INTEGER NOT NULL CHECK(retry_count>=0),recovery INTEGER NOT NULL CHECK(recovery IN(0,1)),state INTEGER NOT NULL CHECK(state IN(0,1,2)),eligible_ms INTEGER CHECK(eligible_ms>=0),queue_order INTEGER NOT NULL CHECK(queue_order>=0),CHECK((state=1 AND eligible_ms IS NOT NULL) OR (state IN(0,2) AND eligible_ms IS NULL))) WITHOUT ROWID; CREATE INDEX unfinished_order ON unfinished(queue_order,url); CREATE INDEX delayed_deadlines ON unfinished(state,eligible_ms,queue_order)");
        impl->prepare();
        for (const auto& url : initial.seen) { auto& s = *impl->insert_seen; s.start(); s.text(1, url.str()); s.done(); }
        std::size_t active = 0;
        for (std::size_t i = 0; i < initial.unfinished.size(); ++i) { impl->work(initial.unfinished[i], i); if (initial.unfinished[i].was_active) { ++active; } }
        auto metadata = CheckpointManager::metadata_only(initial);
        require(active <= metadata.counters.attempts_unresolved, "active input lacks unresolved accounting");
        metadata.counters.attempts_unresolved -= active;
        impl->meta("schema_version", "1"); impl->meta("payload", CheckpointManager::serialize_metadata(metadata)); impl->meta("active_robots", "0");
        transaction.commit(); impl->metadata = std::move(metadata);
        impl->seen_count = initial.seen.size(); impl->work_count = initial.unfinished.size(); impl->active_count = active; impl->validated = true; impl->committed.store(initial.sequence);
        impl->stats.initialization_time = Clock::now() - began;
        return std::unique_ptr<SqliteStore>{new SqliteStore{std::move(impl)}};
    } catch (...) { std::error_code ignored; std::filesystem::remove(path, ignored); throw; }
}
std::unique_ptr<SqliteStore> SqliteStore::open(const std::filesystem::path& path)
{
    const auto began = Clock::now();
    auto impl = std::make_unique<Impl>(path);
    require(scalar(impl->db(), "PRAGMA application_id") == static_cast<std::uint64_t>(application_id), "not a FluxCrawl database");
    require(scalar(impl->db(), "PRAGMA user_version") == schema_version, "unsupported SQLite schema version");
    require(impl->meta("schema_version") == "1", "unsupported/missing schema metadata");
    impl->metadata = CheckpointManager::deserialize_metadata(impl->meta("payload"));
    impl->configure(); impl->prepare(); impl->timings.open = Clock::now() - began;
    return std::unique_ptr<SqliteStore>{new SqliteStore{std::move(impl)}};
}
void SqliteStore::apply(const CrawlMutationBatch& batch) { apply_with_hook(batch, {}); }
void SqliteStore::apply_with_hook(const CrawlMutationBatch& batch, const std::function<void(std::size_t)>& hook)
{
    auto& p = *impl_; std::unique_lock lock{p.mutex};
    if (!p.validated) { lock.unlock(); (void)load(); lock.lock(); }
    require(batch.metadata.sequence == p.metadata->sequence + 1, "out-of-order/repeated transaction sequence");
    require(batch.metadata.seed == p.metadata->seed && CheckpointManager::same_semantics(batch.metadata.options, p.metadata->options)
        && batch.metadata.http.user_agent == p.metadata->http.user_agent
        && batch.metadata.http.connect_timeout == p.metadata->http.connect_timeout && batch.metadata.http.request_timeout == p.metadata->http.request_timeout
        && batch.metadata.http.max_body_bytes == p.metadata->http.max_body_bytes && batch.metadata.http.max_redirects == p.metadata->http.max_redirects
        && batch.metadata.http.use_environment_proxy == p.metadata->http.use_environment_proxy, "semantic/HTTP configuration changed");
    require(batch.metadata.seen.empty() && batch.metadata.unfinished.empty(), "batch must be incremental");
    auto next_metadata = CheckpointManager::metadata_only(batch.metadata);
    const auto began = Clock::now();
    if (batch.metadata.spool_sync) { batch.metadata.spool_sync->sync(); }
    Transaction transaction{p.db()};
    std::size_t seen = p.seen_count, pending = p.work_count, active = p.active_count, applied = 0, row_changes = 0;
    std::unordered_set<Url> new_urls;
    for (const auto& url : batch.discoveries) {
        require(new_urls.insert(url).second, "duplicate discovery mutation");
        auto& s = *p.insert_seen; s.start(); s.text(1, url.str());
        try { s.done(); } catch (...) { if(sqlite3_extended_errcode(p.db())==SQLITE_CONSTRAINT_PRIMARYKEY || sqlite3_extended_errcode(p.db())==SQLITE_CONSTRAINT_UNIQUE) { ++p.stats.seen_constraint_conflicts; } throw; }
        ++seen; row_changes += static_cast<std::size_t>(sqlite3_changes(p.db()));
        if (hook) { hook(++applied); } else { ++applied; }
    }
    std::unordered_set<Url> changed;
    for (const auto& change : batch.changes) {
        require(changed.insert(change.url).second, "uncoalesced duplicate work mutation");
        auto& old = *p.select_work; old.start(); old.text(1, change.url.str()); const bool exists = old.row();
        const auto old_state = exists ? old.integer(0) : 0; old.start();
        require(exists || new_urls.contains(change.url), "work mutation references completed/unknown resource");
        if (exists && old_state == 2) { --active; }
        if (change.unfinished) {
            const auto& w = *change.unfinished;
            require(w.item.url == change.url && w.item.depth <= batch.metadata.options.max_depth
                && w.item.retry_count <= batch.metadata.options.retry.max_retries, "invalid work mutation metadata");
            p.work(w, change.order); if (!exists) { ++pending; } if (w.was_active) { ++active; }
        } else {
            auto& s = *p.delete_work; s.start(); s.text(1, change.url.str()); s.done(); if (exists) { --pending; }
        }
        row_changes += static_cast<std::size_t>(sqlite3_changes(p.db()));
        if (hook) { hook(++applied); } else { ++applied; }
    }
    for (const auto& url : new_urls) { require(changed.contains(url), "discovery lacks unfinished/terminal mutation"); }
    p.aggregate(batch.metadata, seen, pending, active);
    require(batch.metadata.format_version == CrawlCheckpoint::supported_version, "unsupported payload format");
    const auto payload = CheckpointManager::serialize_metadata(batch.metadata); require(payload.size() <= 1024 * 1024, "metadata too large");
    p.meta("payload", payload); p.meta("active_robots", std::to_string(batch.active_robots));
    if (hook) { hook(++applied); }
    transaction.commit();
    p.metadata = std::move(next_metadata); p.seen_count = seen; p.work_count = pending; p.active_count = active;
    const auto elapsed = Clock::now() - began;
    p.stats.seen_insertions += batch.discoveries.size();
    ++p.stats.transactions; p.stats.mutations += batch.operations; p.stats.row_changes += row_changes + 2;
    p.committed.store(batch.metadata.sequence);
    p.stats.last_commit_time = elapsed; p.stats.commit_time += elapsed; p.stats.max_commit_time = std::max(p.stats.max_commit_time, elapsed);
}
CrawlCheckpoint SqliteStore::load()
{
    auto& p = *impl_; std::lock_guard lock{p.mutex}; Transaction transaction{p.db()};
    auto c = CheckpointManager::deserialize_metadata(p.meta("payload"));
    auto began = Clock::now();
    Statement seen{p.db(), "SELECT url FROM seen_urls ORDER BY url"};
    std::size_t seen_count=0; bool seed_found=false;
    const bool external=c.options.dedup.mode!=DedupMode::Memory;
    while (seen.row()) {
        auto url=canonical(seen.string(0)); ++seen_count; seed_found=seed_found || url==c.seed;
        require(!c.options.same_host_only || url.host()==c.seed.host(),"seen URL violates same-host policy");
        if(!external) { c.seen.push_back(std::move(url)); }
    }
    require(seed_found,"seed missing from exact storage");
    if(external) { c.external_seen_count=seen_count; }
    p.timings.seen = Clock::now() - began; began = Clock::now();
    Statement work{p.db(), "SELECT url,depth,retry_count,recovery,state,eligible_ms,queue_order FROM unfinished ORDER BY queue_order,url"};
    std::size_t active = 0;
    while (work.row()) {
        const auto url = canonical(work.string(0)); const auto depth = checked_size(work.integer(1)); const auto retry = checked_size(work.integer(2));
        const auto recovery = work.integer(3), state = work.integer(4);
        require(recovery <= 1 && state <= 2, "invalid work flags");
        std::optional<std::int64_t> deadline;
        if (state == 1) { const auto value = work.integer(5); require(value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), "deadline overflow"); deadline = static_cast<std::int64_t>(value); }
        else { require(sqlite3_column_type(work.statement, 5) == SQLITE_NULL, "unexpected work deadline"); }
        (void)work.integer(6);
        c.unfinished.push_back({{url, depth, retry, false, recovery != 0}, deadline, state == 2}); if (state == 2) { ++active; }
    }
    p.timings.work = Clock::now() - began; began = Clock::now();
    const auto robots_string = p.meta("active_robots"); std::size_t consumed = 0;
    const auto active_robots = std::stoull(robots_string, &consumed); require(consumed == robots_string.size() && !robots_string.empty() && robots_string.front() != '-', "invalid active robots metadata");
    increment(c.counters.attempts_unresolved, active); increment(c.counters.robots_attempts_unresolved, checked_size(active_robots));
    Statement foreign{p.db(), "PRAGMA foreign_key_check"}; require(!foreign.row(), "foreign key violation");
    CheckpointManager::validate(c, std::numeric_limits<std::size_t>::max());
    auto metadata = CheckpointManager::metadata_only(c);
    p.timings.validation = Clock::now() - began; transaction.commit();
    p.metadata = std::move(metadata); p.metadata->counters.attempts_unresolved -= active; p.metadata->counters.robots_attempts_unresolved -= checked_size(active_robots);
    p.seen_count = seen_count; p.work_count = c.unfinished.size(); p.active_count = active; p.validated = true; p.committed.store(c.sequence);
    return c;
}
PersistenceMetrics SqliteStore::metrics() const
{
    std::lock_guard lock{impl_->mutex};
    auto stats = impl_->stats;
    for (const auto& file : {impl_->path, std::filesystem::path{impl_->path.string() + "-wal"}}) {
        std::error_code error; const auto size = std::filesystem::file_size(file, error); if (!error) { stats.file_bytes += static_cast<std::size_t>(size); }
    }
    return stats;
}
StoreLoadTimings SqliteStore::load_timings() const { std::lock_guard lock{impl_->mutex}; return impl_->timings; }
std::uint64_t SqliteStore::committed_sequence() const { return impl_->committed.load(); }
std::size_t SqliteStore::seen_count() const { std::lock_guard lock{impl_->mutex}; return impl_->seen_count; }
bool SqliteStore::seen_contains(const Url& url) { std::lock_guard lock{impl_->mutex}; auto& s=*impl_->lookup_seen; s.start();s.text(1,url.str()); const bool found=s.row();s.start();return found; }
void SqliteStore::visit_seen(const std::function<void(const Url&)>& visitor) { std::lock_guard lock{impl_->mutex}; Statement s{impl_->db(),"SELECT url FROM seen_urls"}; while(s.row()){visitor(canonical(s.string(0)));} }
bool SqliteStore::is_database(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary}; char header[16]{}; file.read(header, sizeof(header));
    return file.gcount() == sizeof(header) && std::memcmp(header, "SQLite format 3\0", 16) == 0;
}
} // namespace fluxcrawl
