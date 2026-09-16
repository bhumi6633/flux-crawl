#include <fluxcrawl/http_response.hpp>

namespace fluxcrawl {

bool is_html_content_type(std::string_view content_type) noexcept
{
    auto type = content_type.substr(0, content_type.find(';'));
    const auto first = type.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return false;
    }
    type = type.substr(first, type.find_last_not_of(" \t") - first + 1);
    constexpr std::string_view html = "text/html";
    if (type.size() != html.size()) {
        return false;
    }
    for (std::size_t index = 0; index < type.size(); ++index) {
        char value = type[index];
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value + ('a' - 'A'));
        }
        if (value != html[index]) {
            return false;
        }
    }
    return true;
}

} // namespace fluxcrawl
