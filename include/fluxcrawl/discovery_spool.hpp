#pragma once
#include <fluxcrawl/crawl_frontier.hpp>
#include <filesystem>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
namespace fluxcrawl {
struct DeferredExtent {
    std::size_t depth;
    std::uint64_t begin, end, count;
    std::uint64_t file_id = 0;
    std::string origin;
};
struct DeferredState {
    std::string directory;
    std::vector<DeferredExtent> extents;
    std::uint64_t next_order = 0;
    std::size_t count() const;
};
// Append-only per-depth disk FIFO. Capture pins immutable byte prefixes; consumed
// bytes remain until the companion directory is explicitly removed offline.
class DiscoverySpool {
public:
    explicit DiscoverySpool(const std::filesystem::path& prefix = {}, bool persistent = false);
    ~DiscoverySpool();
    DiscoverySpool(const DiscoverySpool&) = delete;
    void append(const CrawlItem& item);
    std::optional<CrawlItem> pop(bool fair = true);
    std::size_t size() const;
    DeferredState capture() const;
    void sync() const;
    std::chrono::nanoseconds io_time() const;
    void restore(const DeferredState& state);
    static void validate(const DeferredState& state, std::size_t max_depth, std::optional<std::string_view> required_host = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// A completed response's unique links stream to a temporary disk table,
// preserving first-seen document order without a URL vector or RAM hash set.
class DiskLinks {
public:
    DiskLinks();
    ~DiskLinks();
    void append(const Url& url);
    std::chrono::nanoseconds io_time() const;
    void rewind();
    std::optional<Url> next();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
