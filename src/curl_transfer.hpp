#pragma once

#include <fluxcrawl/http_client.hpp>
#include <curl/curl.h>
#include <array>
#include <memory>

namespace fluxcrawl::detail {

void initialize_http(const HttpClientOptions& options);

struct BodyBuffer {
    std::string bytes;
    std::size_t limit;
    bool too_large = false;
    bool allocation_failed = false;
};

// Keep this object at a stable address. The easy handle references its buffers.
// When used with multi, its owner must detach the handle before destruction.
struct CurlTransfer {
    CurlTransfer(const Url& requested, const HttpClientOptions& options);
    CurlTransfer(const CurlTransfer&) = delete;
    CurlTransfer& operator=(const CurlTransfer&) = delete;
    [[nodiscard]] HttpResult finish(CURLcode code);

    Url url;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    BodyBuffer body;
    std::optional<std::chrono::seconds> retry_after;
    // Declared last: cleanup runs before destruction of callback storage.
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle;
    CURLcode setup = CURLE_OK;
};

} // namespace fluxcrawl::detail
