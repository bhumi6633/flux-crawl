#include "invariants.hpp"
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data),size};
    const auto split=text.find('\n');
    const auto base=fluxcrawl::Url::parse(text.substr(0,split)).value_or(*fluxcrawl::Url::parse("https://example.test/a/b?c=d"));
    const auto reference=split==text.npos?text:text.substr(split+1);
    if (auto url=fluxcrawl::Url::resolve(base,reference)) { verify_url(*url); }
    return 0;
}
