#pragma once
#include <fluxcrawl/url.hpp>
#include <cstdlib>
inline void verify_url(const fluxcrawl::Url& value) {
    const auto again=fluxcrawl::Url::parse(value.canonical());
    if (!again || *again!=value || std::hash<fluxcrawl::Url>{}(*again)!=std::hash<fluxcrawl::Url>{}(value)) { std::abort(); }
    const auto fragment=fluxcrawl::Url::resolve(value,"#removed");
    if (!fragment || *fragment!=value) { std::abort(); }
}
