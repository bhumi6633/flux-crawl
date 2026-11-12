#include <fluxcrawl/robots_policy.hpp>
#include <cstdint>
#include <cstdlib>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data),size};
    if (size>fluxcrawl::RobotsPolicy::max_bytes) { return 0; }
    const auto a=fluxcrawl::RobotsPolicy::parse(text), b=fluxcrawl::RobotsPolicy::parse(text);
    for (auto path:{"/","/public/a","/secret?a=1","/%2F/%7e"}) {
        const auto url=*fluxcrawl::Url::parse(std::string{"http://example.test"}+path);
        for (auto token:{"FluxCrawl","other"}) { if (a.allowed(token,url)!=b.allowed(token,url)) { std::abort(); } }
    }
    return 0;
}
