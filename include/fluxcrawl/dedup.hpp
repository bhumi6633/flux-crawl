#pragma once
#include <chrono>
#include <cstddef>
#include <string_view>
namespace fluxcrawl {
enum class DedupMode { Memory, Sqlite, BloomSqlite };
std::string_view dedup_name(DedupMode mode);
struct DedupOptions {
    DedupMode mode = DedupMode::Memory;
    std::size_t expected_elements = 1000000;
    double false_positive_rate = 0.01;
    std::size_t memory_reserve = 0;
    void validate() const;
};
struct DedupMetrics {
    std::size_t queries=0, insert_attempts=0, accepted=0, confirmed_duplicates=0;
    std::size_t exact_db_queries=0, pending_cache_hits=0, pending_cache_size=0, pending_cache_peak=0;
    std::size_t bloom_queries=0, bloom_definite_misses=0, bloom_maybe_hits=0, exact_checks_after_bloom=0, bloom_false_positives=0;
    std::size_t bloom_bits=0, bloom_bytes=0, bloom_probes=0, bloom_insertions=0, bloom_set_bits=0;
    std::size_t buckets=0, growth_rehashes=0;
    double load_factor=0;
    std::chrono::nanoseconds lookup_time{}, insert_time{}, rebuild_time{}, open_time{};
};
}
