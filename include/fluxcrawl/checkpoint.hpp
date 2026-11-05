#pragma once
#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/http_client.hpp>
#include <filesystem>
#include <functional>
#include <string_view>
namespace fluxcrawl {
struct CheckpointWork {
    CrawlItem item;
    std::optional<std::int64_t> eligible_unix_ms;
    bool was_active = false;
};
struct CrawlCheckpoint {
    static constexpr std::uint32_t supported_version = 1;
    std::uint32_t format_version = supported_version;
    std::string application_version = std::string{version};
    Url seed;
    CrawlOptions options;
    HttpClientOptions http;
    std::uint64_t sequence = 0;
    bool completed = false;
    std::vector<Url> seen;
    std::vector<CheckpointWork> unfinished;
    CrawlResult counters; // Only explicitly named aggregate fields are serialized.
};
class CheckpointManager {
public:
    static constexpr std::size_t max_file_bytes = 64 * 1024 * 1024;
    static void validate(const CrawlCheckpoint& checkpoint);
    [[nodiscard]] static std::string serialize(const CrawlCheckpoint& checkpoint);
    [[nodiscard]] static CrawlCheckpoint deserialize(std::string_view text);
    [[nodiscard]] static CrawlCheckpoint read(const std::filesystem::path& path);
    // before_replace is a deterministic fault-injection hook, also useful to
    // inspect a prepared snapshot. Throwing preserves the previous primary.
    static std::size_t write(const std::filesystem::path& path, const CrawlCheckpoint& checkpoint,
                             const std::function<void()>& before_replace = {});
    [[nodiscard]] static bool same_semantics(const CrawlOptions& a, const CrawlOptions& b);
    [[nodiscard]] static std::int64_t wall_deadline(HostScheduler::Time deadline, HostScheduler::Time steady_now, std::int64_t wall_now_ms);
    [[nodiscard]] static HostScheduler::Time steady_deadline(std::int64_t deadline_ms, HostScheduler::Time steady_now, std::int64_t wall_now_ms);
};
} // namespace fluxcrawl
