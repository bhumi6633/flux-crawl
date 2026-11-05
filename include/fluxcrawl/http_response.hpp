#pragma once

#include <fluxcrawl/url.hpp>

#include <cstddef>
#include <chrono>
#include <string>
#include <string_view>
#include <variant>

namespace fluxcrawl {

[[nodiscard]] bool is_html_content_type(std::string_view content_type) noexcept;

struct HttpResponse {
    Url requested_url;
    Url effective_url;
    long status_code = 0;
    std::string content_type;
    std::string body;
    // Decoded body bytes retained, not compressed wire bytes or headers.
    std::size_t downloaded_bytes = 0;
    std::optional<std::chrono::seconds> retry_after{};
};

enum class HttpErrorCode { CurlError, BodyTooLarge, BufferFailure, InvalidEffectiveUrl, InvalidResponse };

struct HttpError {
    HttpErrorCode code;
    int curl_code;
    std::string message;
};

using HttpResult = std::variant<HttpResponse, HttpError>;

} // namespace fluxcrawl
