#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fluxcrawl {

// A normalized HTTP(S) resource identity. Fragments are deliberately discarded.
class Url {
public:
    [[nodiscard]] static std::optional<Url> parse(std::string_view input);
    [[nodiscard]] static std::optional<Url> resolve(const Url& base, std::string_view reference);

    [[nodiscard]] const std::string& scheme() const noexcept { return scheme_; }
    // IPv6 literals include brackets. Internationalized hosts must use ASCII A-labels.
    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] std::optional<std::uint16_t> port() const noexcept { return port_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& query() const noexcept { return query_; }
    [[nodiscard]] bool has_query() const noexcept { return has_query_; }
    [[nodiscard]] const std::string& str() const noexcept { return canonical_; }
    [[nodiscard]] const std::string& canonical() const noexcept { return canonical_; }
    [[nodiscard]] bool is_http() const noexcept { return scheme_ == "http"; }
    [[nodiscard]] bool is_https() const noexcept { return scheme_ == "https"; }

    bool operator==(const Url& other) const noexcept { return canonical_ == other.canonical_; }

private:
    Url() = default;

    std::string scheme_;
    std::string host_;
    std::optional<std::uint16_t> port_;
    std::string path_;
    std::string query_;
    bool has_query_ = false;
    std::string canonical_;
};

} // namespace fluxcrawl

template <>
struct std::hash<fluxcrawl::Url> {
    std::size_t operator()(const fluxcrawl::Url& url) const noexcept
    {
        return std::hash<std::string>{}(url.canonical());
    }
};
