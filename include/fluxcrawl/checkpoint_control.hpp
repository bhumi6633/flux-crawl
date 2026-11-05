#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
namespace fluxcrawl {
struct CrawlCheckpoint;
struct CheckpointControl {
    std::filesystem::path path;
    std::size_t every = 100; // Resource attempts, checkpoints at safe boundaries.
    // Optional graceful, deterministic library stop; execution setting, not persisted.
    std::optional<std::size_t> stop_after_attempts;
    std::function<void(const CrawlCheckpoint&)> on_snapshot;
};
} // namespace fluxcrawl
