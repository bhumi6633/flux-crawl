#include <fluxcrawl/http_client.hpp>
#include "curl_transfer.hpp"

#include <utility>

namespace fluxcrawl {

HttpClient::HttpClient(HttpClientOptions options) : options_{std::move(options)}
{
    detail::initialize_http(options_);
}

HttpResult HttpClient::fetch(const Url& url) const
{
    return fetch(url, options_.max_body_bytes);
}
HttpResult HttpClient::fetch(const Url& url, std::size_t body_limit) const
{
    auto request_options = options_;
    request_options.max_body_bytes = body_limit;
    detail::CurlTransfer transfer{url, request_options};
    return transfer.finish(transfer.setup == CURLE_OK
        ? curl_easy_perform(transfer.handle.get()) : transfer.setup);
}

} // namespace fluxcrawl
