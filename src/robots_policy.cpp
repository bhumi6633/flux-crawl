#include <fluxcrawl/robots_policy.hpp>
#include <algorithm>
#include <stdexcept>

namespace fluxcrawl {
namespace {
std::string_view trim(std::string_view s)
{
    const auto start = s.find_first_not_of(" \t\r");
    return start == s.npos ? std::string_view{} : s.substr(start, s.find_last_not_of(" \t\r") - start + 1);
}
std::string lower(std::string_view s)
{
    std::string result{s};
    for (auto& c : result) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); } }
    return result;
}
int hex(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}
std::string octets(std::string_view s)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            c = static_cast<unsigned char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
            if (!unreserved) { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
            else { result += static_cast<char>(c); }
            i += 2;
        } else if (c >= 128) {
            result += '%'; result += digits[c >> 4]; result += digits[c & 15];
        } else { result += static_cast<char>(c); }
    }
    return result;
}
}
RobotsPolicy RobotsPolicy::parse(std::string_view text)
{
    if (text.size() > max_bytes) { throw std::invalid_argument("Robots policy exceeds 512 KiB"); }
    if (text.starts_with("\xEF\xBB\xBF")) { text.remove_prefix(3); }
    RobotsPolicy policy;
    Group* group = nullptr;
    bool has_rules = false;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        line = trim(line.substr(0, line.find('#')));
        const auto colon = line.find(':');
        if (colon == line.npos) { continue; }
        const auto key = lower(trim(line.substr(0, colon)));
        const auto value = trim(line.substr(colon + 1));
        if (key == "user-agent") {
            if (value.empty()) { continue; }
            if (!group || has_rules) {
                policy.groups_.emplace_back(); group = &policy.groups_.back(); has_rules = false;
            }
            group->agents.push_back(lower(value));
        } else if ((key == "allow" || key == "disallow") && group) {
            has_rules = true; // Empty rules still separate subsequent groups.
            if (value.empty() || value.front() != '/') { continue; }
            group->rules.push_back({octets(value), key == "allow"});
        }
    }
    return policy;
}
bool RobotsPolicy::allowed(std::string_view product_token, const Url& url) const
{
    const auto token = lower(product_token);
    const bool specific = std::any_of(groups_.begin(), groups_.end(), [&](const Group& group) {
        return std::find(group.agents.begin(), group.agents.end(), token) != group.agents.end();
    });
    auto path = url.path();
    if (url.has_query()) { path += '?' + url.query(); }
    path = octets(path);
    bool result = true;
    std::size_t longest = 0;
    for (const auto& group : groups_) {
        const auto agent = specific ? std::string_view{token} : std::string_view{"*"};
        if (std::find(group.agents.begin(), group.agents.end(), agent) == group.agents.end()) { continue; }
        for (const auto& rule : group.rules) {
            if (path.starts_with(rule.path) && (rule.path.size() > longest || (rule.path.size() == longest && rule.allow))) {
                longest = rule.path.size(); result = rule.allow;
            }
        }
    }
    return result;
}
} // namespace fluxcrawl
