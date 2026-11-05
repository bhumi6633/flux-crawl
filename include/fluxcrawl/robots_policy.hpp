#pragma once
#include <fluxcrawl/url.hpp>
#include <string>
#include <vector>

namespace fluxcrawl {
// Bounded, literal prefix subset. No wildcard/$ expansion, sitemap or Crawl-delay.
class RobotsPolicy {
public:
    static constexpr std::size_t max_bytes = 512 * 1024;
    [[nodiscard]] static RobotsPolicy parse(std::string_view text);
    [[nodiscard]] bool allowed(std::string_view product_token, const Url& url) const;
private:
    struct Rule { std::string path; bool allow; };
    struct Group { std::vector<std::string> agents; std::vector<Rule> rules; };
    std::vector<Group> groups_;
};
} // namespace fluxcrawl
