#include <fluxcrawl/url.hpp>

#include <curl/curl.h>

#include <charconv>
#include <memory>

namespace fluxcrawl {
namespace {

bool is_hex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
        || (value >= 'A' && value <= 'F');
}

bool valid_uri_text(std::string_view text)
{
    constexpr std::string_view punctuation = "-._~:/?#[]@!$&'()*+,;=";
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (value == '%') {
            if (text.size() - index < 3 || !is_hex(text[index + 1]) || !is_hex(text[index + 2])) {
                return false;
            }
            index += 2;
        } else if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                     || (value >= '0' && value <= '9')
                     || punctuation.find(value) != std::string_view::npos)) {
            return false;
        }
    }
    return true;
}

std::string lowercase(std::string_view text)
{
    std::string result{text};
    for (char& value : result) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value + ('a' - 'A'));
        }
    }
    return result;
}

std::optional<std::string> part(CURLU* handle, CURLUPart component)
{
    char* raw = nullptr;
    const auto status = curl_url_get(handle, component, &raw, 0);
    const std::unique_ptr<char, decltype(&curl_free)> value{raw, curl_free};
    if (status != CURLUE_OK) {
        return std::nullopt;
    }
    return std::string{value.get()};
}

} // namespace

std::optional<Url> Url::parse(std::string_view input)
{
    if (!valid_uri_text(input)) {
        return std::nullopt;
    }
    const auto separator = input.find("://");
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto scheme = lowercase(input.substr(0, separator));
    if (scheme != "http" && scheme != "https") {
        return std::nullopt;
    }
    const auto remainder = input.substr(separator + 3);
    const auto authority = remainder.substr(0, remainder.find_first_of("/?#"));
    // Reject curl's permissive slash/empty-port forms and identities we do not model.
    if (authority.empty() || authority.back() == ':'
        || authority.find('@') != std::string_view::npos
        || authority.find('%') != std::string_view::npos) {
        return std::nullopt;
    }

    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> handle{curl_url(), curl_url_cleanup};
    const std::string text{input};
    if (!handle || curl_url_set(handle.get(), CURLUPART_URL, text.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) {
        return std::nullopt;
    }
    const auto host = part(handle.get(), CURLUPART_HOST);
    const auto path = part(handle.get(), CURLUPART_PATH);
    if (!host || host->empty() || !path) {
        return std::nullopt;
    }

    Url result;
    result.scheme_ = scheme;
    result.host_ = lowercase(*host);
    result.path_ = path->empty() ? "/" : *path;
    if (const auto port = part(handle.get(), CURLUPART_PORT)) {
        std::uint16_t number = 0;
        const auto [end, error] = std::from_chars(port->data(), port->data() + port->size(), number);
        if (error != std::errc{} || end != port->data() + port->size()) {
            return std::nullopt;
        }
        if (!((scheme == "http" && number == 80) || (scheme == "https" && number == 443))) {
            result.port_ = number;
        }
    }

    // Preserve query bytes and distinguish an empty query from no query, even on older curl.
    const auto resource = input.substr(0, input.find('#'));
    const auto query_start = resource.find('?');
    result.has_query_ = query_start != std::string_view::npos;
    if (result.has_query_) {
        result.query_ = resource.substr(query_start + 1);
    }
    result.canonical_ = result.scheme_ + "://" + result.host_;
    if (result.port_) {
        result.canonical_ += ':' + std::to_string(*result.port_);
    }
    result.canonical_ += result.path_;
    if (result.has_query_) {
        result.canonical_ += '?' + result.query_;
    }
    return result;
}

std::optional<Url> Url::resolve(const Url& base, std::string_view reference)
{
    if (!valid_uri_text(reference)) {
        return std::nullopt;
    }
    // RFC 3986 section 5.2: replace scheme/authority, or merge paths and inherit query.
    const auto first_delimiter = reference.find_first_of(":/?#");
    if (first_delimiter != std::string_view::npos && reference[first_delimiter] == ':') {
        return parse(reference);
    }
    if (reference.starts_with("//")) {
        return parse(base.scheme_ + ':' + std::string{reference});
    }
    const auto resource = reference.substr(0, reference.find('#'));
    if (resource.empty()) {
        return base;
    }
    const auto query_start = resource.find('?');
    const auto reference_path = resource.substr(0, query_start);
    std::string path;
    if (reference_path.empty()) {
        path = base.path_;
    } else if (reference_path.starts_with('/')) {
        path = reference_path;
    } else {
        path = base.path_.substr(0, base.path_.rfind('/') + 1);
        path += reference_path;
    }

    std::string target = base.scheme_ + "://" + base.host_;
    if (base.port_) {
        target += ':' + std::to_string(*base.port_);
    }
    target += path;
    if (query_start != std::string_view::npos) {
        target += resource.substr(query_start);
    }
    return parse(target);
}

} // namespace fluxcrawl
