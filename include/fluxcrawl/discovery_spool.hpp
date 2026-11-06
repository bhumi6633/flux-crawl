#pragma once
#include <fluxcrawl/crawl_frontier.hpp>
#include <filesystem>
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
    std::optional<CrawlItem> pop();
    std::size_t size() const;
    DeferredState capture() const;
    void sync() const;
    void restore(const DeferredState& state);
    static void validate(const DeferredState& state, std::size_t max_depth);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// A completed response's links stream to a temporary file, not a URL vector.
class DiskLinks {
public:
    DiskLinks();
    ~DiskLinks();
    void append(const Url& url);
    void rewind();
    std::optional<Url> next();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
