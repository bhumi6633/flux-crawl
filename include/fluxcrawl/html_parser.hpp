#pragma once

#include <fluxcrawl/url.hpp>

#include <string_view>
#include <vector>

namespace fluxcrawl {

class HtmlLinkExtractor {
public:
    // Unique HTTP(S) anchor targets in first-seen order, using the caller's base.
    // Empty/unparseable input returns no links. HTML <base> elements are ignored.
    [[nodiscard]] std::vector<Url> extract_links(std::string_view html, const Url& base_url) const;
};

} // namespace fluxcrawl
