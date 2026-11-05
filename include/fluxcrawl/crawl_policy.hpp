#pragma once
#include <fluxcrawl/crawl_types.hpp>
#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/robots_policy.hpp>
#include <unordered_map>

namespace fluxcrawl {
// Orchestration policy, separate from scheduler eligibility. All calls must be
// serialized by the caller, just like CrawlState and HostScheduler.
class CrawlPolicy {
public:
    enum class State { Fetching, Ready, Unavailable }; // Missing entry is Unknown.
    explicit CrawlPolicy(bool enabled) : enabled_{enabled} {}
    void schedule(CrawlState& discovery, HostScheduler& scheduler, CrawlResult& result);
    void resolve(const CrawlRecord& bootstrap, std::optional<RobotsPolicy> rules,
                 HostScheduler& scheduler, CrawlResult& result);
    void accept(HostScheduler::Work work, HostScheduler& scheduler, CrawlResult& result);
    [[nodiscard]] std::vector<HostScheduler::Work> snapshot() const;
    [[nodiscard]] std::size_t held() const noexcept;
private:
    struct Entry {
        State state = State::Fetching;
        RobotsPolicy rules;
        bool deny_all = false;
        bool reusable = false;
        std::vector<HostScheduler::Work> waiting;
    };
    void admit(HostScheduler::Work work, const Entry& entry, HostScheduler& scheduler, CrawlResult& result);
    bool enabled_;
    std::unordered_map<HostKey, Entry, HostKeyHash> entries_;
};
} // namespace fluxcrawl
