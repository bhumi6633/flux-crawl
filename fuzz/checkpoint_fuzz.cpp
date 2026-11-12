#include <fluxcrawl/checkpoint.hpp>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data),size};
    // Invalid states must throw before they can be handed to an engine.
    try {
        const auto state=fluxcrawl::CheckpointManager::deserialize(text);
        const auto canonical=fluxcrawl::CheckpointManager::serialize(state);
        const auto again=fluxcrawl::CheckpointManager::deserialize(canonical);
        if (fluxcrawl::CheckpointManager::serialize(again)!=canonical) { std::abort(); }
    } catch (const std::runtime_error&) {} catch (const std::invalid_argument&) {} catch (const std::length_error&) {}
    return 0;
}
