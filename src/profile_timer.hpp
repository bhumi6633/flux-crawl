#pragma once
#include <fluxcrawl/profiling.hpp>

namespace fluxcrawl::detail {
// No clock reads when disabled; no allocations, atomics, or profiler locks.
class ProfileTimer {
public:
    explicit ProfileTimer(ProfileDuration* destination) : destination_{destination}
    {
        if (destination_) { started_ = std::chrono::steady_clock::now(); }
    }
    ~ProfileTimer()
    {
        if (destination_) { *destination_ += std::chrono::steady_clock::now() - started_; }
    }
    ProfileTimer(const ProfileTimer&) = delete;
    ProfileTimer& operator=(const ProfileTimer&) = delete;
private:
    ProfileDuration* destination_;
    std::chrono::steady_clock::time_point started_{};
};
} // namespace fluxcrawl::detail
