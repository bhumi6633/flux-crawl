#pragma once
#include <chrono>
#include <cstddef>
#include <filesystem>
namespace fluxcrawl {
enum class PersistenceMode { None, Snapshot, Sqlite };
struct SqliteControl {
    std::filesystem::path path;
    std::size_t batch_size = 100; // Logical mutations, coalesced per resource.
    bool resume = false;
};
struct PersistenceMetrics {
    std::size_t buffered_mutations_peak = 0, queued_batches_peak = 0;
    std::size_t seen_insertions = 0, seen_constraint_conflicts = 0;
    std::size_t transactions = 0, mutations = 0, row_changes = 0, file_bytes = 0;
    std::chrono::nanoseconds commit_time{}, last_commit_time{}, max_commit_time{}, initialization_time{};
};
} // namespace fluxcrawl
