#pragma once
#include <cstddef>
#include <stdexcept>
namespace certification {
// Test-only deterministic fail-after-N plan. Never used by production randomness.
struct FaultPlan {
    std::size_t remaining=0;
    void arm(std::size_t hit=1) { remaining=hit; }
    bool fires() { return remaining && --remaining==0; }
    void hit() { if (fires()) { throw std::runtime_error("injected certification fault"); } }
};
}
