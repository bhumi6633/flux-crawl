#include "curl_transfer.hpp"
#include <fluxcrawl/retry_policy.hpp>

#include <curl/curl.h>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace fluxcrawl::detail {
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

std::size_t header(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) { return 0; }
    const auto length = size * count;
    auto& transfer = *static_cast<CurlTransfer*>(user);
    std::string_view line{data, length};
    // Reset at every response block, including redirects and informational responses.
    if (line.starts_with("HTTP/")) { transfer.retry_after.reset(); }
    constexpr std::string_view name = "retry-after:";
    if (line.size() >= name.size()) {
        bool match = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            auto ch = line[i];
            if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch + ('a' - 'A')); }
            if (ch != name[i]) { match = false; break; }
        }
        if (match) {
            const auto parsed = parse_retry_after(line.substr(name.size()));
            if (parsed && (!transfer.retry_after || *parsed > *transfer.retry_after)) { transfer.retry_after = parsed; }
        }
    }
    return length;
}

HttpError curl_error(CURLcode code, const char* detail = nullptr)
{
    return {HttpErrorCode::CurlError, static_cast<int>(code),
            detail && *detail ? detail : curl_easy_strerror(code)};
}

} // namespace

void initialize_http(const HttpClientOptions& options)
{
    const auto valid_timeout = [](std::chrono::milliseconds timeout) {
        return timeout.count() > 0 && timeout.count() <= std::numeric_limits<long>::max();
    };
    if (!valid_timeout(options.connect_timeout) || !valid_timeout(options.request_timeout)
        || options.max_redirects < 0 || options.user_agent.empty()
        || options.user_agent.find_first_of("\r\n") != std::string::npos
        || options.user_agent.find('\0') != std::string::npos) {
        throw std::invalid_argument("Invalid HTTP client timeouts, redirect limit, or user agent");
    }
    // C++ guarantees one-time initialization; no easy handle is shared globally.
    static const CurlRuntime runtime;
    (void)runtime;
}

CurlTransfer::CurlTransfer(const Url& requested, const HttpClientOptions& options)
    : url{requested}, body{{}, options.max_body_bytes}, handle{curl_easy_init(), curl_easy_cleanup}
{
    if (!handle) {
        setup = CURLE_FAILED_INIT;
        return;
    }

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
    set(CURLOPT_MAXREDIRS, options.max_redirects);
    set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connect_timeout.count()));
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(options.request_timeout.count()));
    set(CURLOPT_NOSIGNAL, 1L);
    if (!options.use_environment_proxy) { set(CURLOPT_PROXY, ""); }
    set(CURLOPT_USERAGENT, options.user_agent.c_str());
    set(CURLOPT_ACCEPT_ENCODING, "");
    set(CURLOPT_WRITEFUNCTION, &receive);
    set(CURLOPT_WRITEDATA, &body);
    set(CURLOPT_HEADERFUNCTION, &header);
    set(CURLOPT_HEADERDATA, this);
}

HttpResult CurlTransfer::finish(CURLcode code)
{
    if (setup != CURLE_OK) {
        return curl_error(setup, error_buffer.data());
    }
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
    return HttpResponse{url, *final_url, status, content_type ? content_type : "", std::move(body.bytes), downloaded, retry_after};
}

} // namespace fluxcrawl::detail
