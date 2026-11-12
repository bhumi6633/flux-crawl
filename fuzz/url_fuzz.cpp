#include "invariants.hpp"
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (auto url=fluxcrawl::Url::parse({reinterpret_cast<const char*>(data),size})) { verify_url(*url); }
    return 0;
}
