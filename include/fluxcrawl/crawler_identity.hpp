#pragma once
#include <fluxcrawl/version.hpp>
#include <string>
#include <string_view>

namespace fluxcrawl {
// Fixed default identity. Custom HTTP headers do not change the robots token.
struct CrawlerIdentity {
    static constexpr std::string_view product_token = "FluxCrawl";
    std::string user_agent = "FluxCrawl/" + std::string{version};
};
} // namespace fluxcrawl
