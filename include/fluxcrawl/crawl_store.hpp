#pragma once
#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/persistence_control.hpp>
#include <memory>
namespace fluxcrawl {
struct WorkMutation {
    Url url;
    std::optional<CheckpointWork> unfinished; // Empty means terminal deletion.
    std::uint64_t order = 0;
};
struct CrawlMutationBatch {
    CrawlCheckpoint metadata; // No whole-state seen/frontier copies.
    std::vector<Url> discoveries;
    std::vector<WorkMutation> changes;
    std::size_t operations = 0;
    std::size_t active_robots = 0;
};
class CrawlStore {
public:
    virtual ~CrawlStore() = default;
    virtual void apply(const CrawlMutationBatch& batch) = 0;
    [[nodiscard]] virtual CrawlCheckpoint load() = 0;
    [[nodiscard]] virtual PersistenceMetrics metrics() const = 0;
};
} // namespace fluxcrawl
