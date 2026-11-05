#pragma once

#include <fluxcrawl/http_response.hpp>
#include <fluxcrawl/crawler_identity.hpp>

#include <chrono>

namespace fluxcrawl {

struct HttpClientOptions {
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{30000};
    long max_redirects = 10;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::string user_agent = CrawlerIdentity{}.user_agent;
    bool use_environment_proxy = true;
};

class HttpClient {
public:
    // Invalid options or global initialization failure throw at construction.
    explicit HttpClient(HttpClientOptions options = {});
    [[nodiscard]] const HttpClientOptions& options() const noexcept { return options_; }
    [[nodiscard]] HttpResult fetch(const Url& url) const;
    [[nodiscard]] HttpResult fetch(const Url& url, std::size_t body_limit) const;

private:
    HttpClientOptions options_;
};

} // namespace fluxcrawl
