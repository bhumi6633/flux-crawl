#pragma once
#include <fluxcrawl/crawl_store.hpp>
#include <functional>
namespace fluxcrawl {
struct StoreLoadTimings {
    std::chrono::nanoseconds open{}, seen{}, work{}, validation{};
};
// One logical owner/connection. Runtime serializes every call; POSIX ownership
// locking rejects another FluxCrawl owner. Destruction never commits a batch.
class SqliteStore final : public CrawlStore {
public:
    static constexpr int schema_version = 1;
    static std::unique_ptr<SqliteStore> create(const std::filesystem::path& path, const CrawlCheckpoint& initial);
    static std::unique_ptr<SqliteStore> open(const std::filesystem::path& path);
    ~SqliteStore() override;
    void apply(const CrawlMutationBatch& batch) override;
    // Fail after a prepared mutation, before COMMIT; used for atomicity tests.
    void apply_with_hook(const CrawlMutationBatch& batch, const std::function<void(std::size_t)>& hook);
    [[nodiscard]] CrawlCheckpoint load() override;
    [[nodiscard]] PersistenceMetrics metrics() const override;
    [[nodiscard]] StoreLoadTimings load_timings() const;
    [[nodiscard]] static bool is_database(const std::filesystem::path& path);
private:
    struct Impl;
    explicit SqliteStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace fluxcrawl
