#pragma once
#include <fluxcrawl/crawl_store.hpp>
#include <fluxcrawl/persistence_events.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace fluxcrawl::detail {
class PersistenceJournal final : public CrawlMutationSink {
public:
    void discovered(const CrawlItem& item) override { discoveries_.push_back(item.url); ++operations_; work(item, {}, false); }
    void work(const CrawlItem& item, std::optional<std::chrono::steady_clock::time_point> deadline, bool active) override {
        if (item.policy_request) { return; }
        if (active) { active_.insert(item.url); } else { active_.erase(item.url); }
        const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto when = deadline ? std::optional{CheckpointManager::wall_deadline(*deadline, std::chrono::steady_clock::now(), wall)} : std::nullopt;
        changes_.insert_or_assign(item.url, WorkMutation{item.url, CheckpointWork{item, when, active}, ++order_});
        ++operations_;
    }
    void completed(const CrawlItem& item) override {
        if (item.policy_request) { return; }
        active_.erase(item.url); changes_.insert_or_assign(item.url, WorkMutation{item.url, {}, 0}); ++operations_;
    }
    void activity() override { ++operations_; }
    std::size_t operations() const noexcept { return operations_; }
    std::size_t active() const noexcept { return active_.size(); }
    CrawlMutationBatch batch(CrawlCheckpoint metadata, std::size_t active_robots) const {
        CrawlMutationBatch batch{std::move(metadata), discoveries_, {}, operations_, active_robots};
        batch.changes.reserve(changes_.size());
        for (const auto& [url, change] : changes_) { (void)url; batch.changes.push_back(change); }
        std::sort(batch.changes.begin(), batch.changes.end(), [](const auto& a, const auto& b) { return a.url.str() < b.url.str(); });
        return batch;
    }
    void clear() { discoveries_.clear(); changes_.clear(); operations_ = 0; }
private:
    std::vector<Url> discoveries_;
    std::unordered_map<Url, WorkMutation> changes_;
    std::unordered_set<Url> active_;
    std::size_t operations_ = 0;
    std::uint64_t order_ = 0;
};
} // namespace fluxcrawl::detail
