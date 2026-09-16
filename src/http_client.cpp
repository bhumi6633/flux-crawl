#include <fluxcrawl/http_client.hpp>

#include <curl/curl.h>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace fluxcrawl {
namespace {

class CurlRuntime {
public:
    CurlRuntime()
    {
        const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error(curl_easy_strerror(code));
        }
    }
    ~CurlRuntime() { curl_global_cleanup(); }
    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
};

struct BodyBuffer {
    std::string bytes;
    std::size_t limit;
    bool too_large = false;
    bool allocation_failed = false;
};

std::size_t receive(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& buffer = *static_cast<BodyBuffer*>(user);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        buffer.too_large = true;
        return 0;
    }
    const auto length = size * count;
    if (length > buffer.limit - buffer.bytes.size()) {
        buffer.too_large = true;
        return 0;
    }
    try {
        if (length != 0) {
            buffer.bytes.append(data, length);
        }
    } catch (...) {
        // Exceptions must never cross the C callback boundary.
        buffer.allocation_failed = true;
        return 0;
    }
    return length;
}

HttpError curl_error(CURLcode code, const char* detail = nullptr)
{
    return {HttpErrorCode::CurlError, static_cast<int>(code),
            detail && *detail ? detail : curl_easy_strerror(code)};
}

} // namespace

HttpClient::HttpClient(HttpClientOptions options) : options_{std::move(options)}
{
    const auto valid_timeout = [](std::chrono::milliseconds timeout) {
        return timeout.count() > 0 && timeout.count() <= std::numeric_limits<long>::max();
    };
    if (!valid_timeout(options_.connect_timeout) || !valid_timeout(options_.request_timeout)
        || options_.max_redirects < 0 || options_.user_agent.empty()
        || options_.user_agent.find_first_of("\r\n") != std::string::npos
        || options_.user_agent.find('\0') != std::string::npos) {
        throw std::invalid_argument("Invalid HTTP client timeouts, redirect limit, or user agent");
    }
    // C++ guarantees one-time initialization; no easy handle is shared globally.
    static const CurlRuntime runtime;
    (void)runtime;
}

HttpResult HttpClient::fetch(const Url& url) const
{
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    BodyBuffer body{{}, options_.max_body_bytes};
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{curl_easy_init(), curl_easy_cleanup};
    if (!handle) {
        return curl_error(CURLE_FAILED_INIT);
    }

    CURLcode setup = CURLE_OK;
    const auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) {
            setup = curl_easy_setopt(handle.get(), option, value);
        }
    };
    set(CURLOPT_ERRORBUFFER, error_buffer.data());
    set(CURLOPT_URL, url.canonical().c_str());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, options_.max_redirects);
    set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options_.connect_timeout.count()));
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(options_.request_timeout.count()));
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_USERAGENT, options_.user_agent.c_str());
    set(CURLOPT_ACCEPT_ENCODING, "");
    set(CURLOPT_WRITEFUNCTION, &receive);
    set(CURLOPT_WRITEDATA, &body);
    if (setup != CURLE_OK) {
        return curl_error(setup, error_buffer.data());
    }
    const auto code = curl_easy_perform(handle.get());
    if (body.too_large) {
        return HttpError{HttpErrorCode::BodyTooLarge, static_cast<int>(code), "Response body exceeds configured byte limit"};
    }
    if (body.allocation_failed) {
        return HttpError{HttpErrorCode::BufferFailure, static_cast<int>(code), "Unable to store response body"};
    }
    if (code != CURLE_OK) {
        return curl_error(code, error_buffer.data());
    }

    long status = 0;
    char* effective = nullptr;
    char* content_type = nullptr;
    for (const auto info_code : {
             curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status),
             curl_easy_getinfo(handle.get(), CURLINFO_EFFECTIVE_URL, &effective),
             curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &content_type)}) {
        if (info_code != CURLE_OK) {
            return curl_error(info_code);
        }
    }
    const auto final_url = effective ? Url::parse(effective) : std::nullopt;
    if (!final_url) {
        return HttpError{HttpErrorCode::InvalidEffectiveUrl, 0, "Effective URL is outside the supported URL model"};
    }
    if (status == 0) {
        return HttpError{HttpErrorCode::InvalidResponse, 0, "No HTTP status received"};
    }
    const auto downloaded = body.bytes.size();
    return HttpResponse{url, *final_url, status, content_type ? content_type : "", std::move(body.bytes), downloaded};
}

} // namespace fluxcrawl
