#pragma once

#include <fluxcrawl/url.hpp>

#include <string_view>
#include <functional>
#include <vector>

namespace fluxcrawl {

class HtmlLinkExtractor {
public:
    HtmlLinkExtractor();
    // DOM traversal in document order; duplicates flow to global admission dedup.
    // No XPath node array or document-sized URL/reference sets are retained.
    void for_each_link(std::string_view html, const Url& base_url, const std::function<void(const Url&)>& visitor) const;
    // Unique HTTP(S) anchor targets in first-seen order, using the caller's base.
    // Empty/unparseable input returns no links. HTML <base> elements are ignored.
    [[nodiscard]] std::vector<Url> extract_links(std::string_view html, const Url& base_url) const;
};

} // namespace fluxcrawl
