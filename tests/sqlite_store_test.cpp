#include <fluxcrawl/sqlite_store.hpp>
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace fluxcrawl;
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
template<class F> void rejects(F f) { bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; } check(rejected, "Invalid SQLite operation accepted"); }
Url url(const char* s) { return Url::parse(s).value(); }
CrawlCheckpoint initial()
{
    auto seed = url("http://a.test/"); CrawlCheckpoint c{.seed = seed}; c.options.retry.max_retries = 2;
    c.seen = {seed}; c.unfinished = {{{seed, 0}, {}, false}}; c.counters.urls_discovered = c.counters.urls_pending = 1;
    return c;
}
CrawlMutationBatch discover(CrawlCheckpoint metadata, Url u)
{
    metadata = CheckpointManager::metadata_only(metadata); ++metadata.sequence; ++metadata.counters.urls_discovered; ++metadata.counters.urls_pending; metadata.completed = false;
    return {metadata, {u}, {{u, CheckpointWork{{u, 1}, {}, false}, metadata.sequence}}, 2, 0};
}
void sql(const std::filesystem::path& path, const char* statement)
{
    sqlite3* db = nullptr; check(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "test DB open");
    const auto code = sqlite3_exec(db, statement, nullptr, nullptr, nullptr); sqlite3_close(db);
    check(code == SQLITE_OK, "test SQL mutation failed");
}
int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const auto directory = std::filesystem::path{argv[1]} / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    try {
        const auto file = directory / "crawl.db"; auto c = initial(); auto store = SqliteStore::create(file, c);
        check(SqliteStore::is_database(file), "SQLite file detection");
        rejects([&] { (void)SqliteStore::create(file, c); });
        rejects([&] { auto other = SqliteStore::open(file); (void)other->load(); });
        auto a = url("http://a.test/a'b?x='"); auto batch = discover(c, a);
        rejects([&] { store->apply_with_hook(batch, [](std::size_t n) { if (n == 1) { throw std::runtime_error("failure between seen and unfinished"); } }); });
        check(store->load().seen.size() == 1, "transaction rolled back seen insertion");
        store->apply(batch); c = batch.metadata;
        check(store->load().seen.size() == 2 && store->load().unfinished.size() == 2, "discovery durable and SQL binding safe");
        rejects([&] { store->apply(batch); });
        auto duplicate = discover(c, a); rejects([&] { store->apply(duplicate); });
        check(store->load().seen.size() == 2, "unique constraint prevents duplicate durable identity");
        // Active reservations remain recoverable with their original budget charge.
        ++c.sequence; ++c.counters.pages_attempted;
        store->apply({c, {}, {{c.seed, CheckpointWork{{c.seed, 0}, {}, true}, 3}}, 1, 0});
        auto active = store->load(); check(active.unfinished[0].was_active || active.unfinished[1].was_active, "active work persisted");
        check(active.counters.attempts_unresolved == 1 && active.counters.pages_attempted == 1, "active recovery budget");
        ++c.sequence; ++c.counters.responses_received; ++c.counters.resources_completed; --c.counters.urls_pending;
        store->apply({c, {}, {{c.seed, {}, 0}}, 1, 0});
        auto completed = store->load(); check(completed.seen.size() == 2 && completed.unfinished.size() == 1 && completed.unfinished[0].item.url == a, "completed seed not restored");
        ++c.sequence; ++c.counters.pages_attempted;
        store->apply({c, {}, {{a, CheckpointWork{{a, 1}, {}, true}, 4}}, 1, 0});
        ++c.sequence; ++c.counters.responses_received; ++c.counters.retries_scheduled; ++c.counters.retries_cancelled;
        const auto deadline = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 2000;
        store->apply({c, {}, {{a, CheckpointWork{{a, 1, 1}, deadline, false}, 5}}, 1, 0});
        store.reset(); store = SqliteStore::open(file);
        const auto delayed = store->load(); check(delayed.unfinished.size() == 1 && delayed.unfinished[0].eligible_unix_ms == deadline && delayed.unfinished[0].item.retry_count == 1, "retry replaces active atomically");
        ++c.sequence; ++c.counters.pages_attempted; ++c.counters.retries_attempted; --c.counters.retries_cancelled;
        store->apply({c, {}, {{a, CheckpointWork{{a, 1, 1}, {}, true}, 6}}, 1, 0});
        ++c.sequence; ++c.counters.responses_received; ++c.counters.resources_completed; --c.counters.urls_pending; ++c.counters.responses_recovered_after_retry; c.completed = true;
        CrawlMutationBatch final{c, {}, {{a, {}, 0}}, 1, 0};
        rejects([&] { store->apply_with_hook(final, [](std::size_t) { throw std::runtime_error("final flush failure"); }); });
        check(!store->load().completed, "failed final transaction does not mark completed");
        store->apply(final); check(store->load().completed && store->load().unfinished.empty(), "durable completed marker");
        store.reset();
        // A committed prefix survives, while an unsubmitted memory batch does not.
        const auto buffered = directory / "buffered.db"; auto buffer_store = SqliteStore::create(buffered, initial());
        auto prefix = discover(initial(), a); buffer_store->apply(prefix);
        auto suffix = discover(prefix.metadata, url("http://b.test/tail")); buffer_store.reset();
        buffer_store = SqliteStore::open(buffered); check(buffer_store->load().seen.size() == 2, "uncommitted suffix absent");
        buffer_store->apply(suffix); buffer_store.reset(); buffer_store = SqliteStore::open(buffered);
        check(buffer_store->load().seen.size() == 3, "explicit flush survives close/reopen"); buffer_store.reset();
        const auto corrupt = [&](const char* name, const char* statement) {
            const auto target = directory / name; std::filesystem::copy_file(buffered, target); sql(target, statement);
            rejects([&] { auto bad = SqliteStore::open(target); (void)bad->load(); });
        };
        corrupt("version.db", "PRAGMA user_version=2");
        corrupt("metadata.db", "DELETE FROM crawl_meta WHERE key='payload'");
        corrupt("url.db", "UPDATE seen_urls SET url='not-a-url' WHERE url='http://a.test/'");
        corrupt("retry.db", "UPDATE unfinished SET retry_count=99");
        corrupt("orphan.db", "INSERT INTO unfinished VALUES('http://missing.test/',0,0,0,0,NULL,99)");
        sql(buffered, "PRAGMA integrity_check");
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "SQLite schema, transactions, uniqueness, rollback, recovery and corruption checks passed\n";
}
