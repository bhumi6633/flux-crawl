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
    detail::CurlTransfer transfer{url, options_};
    return transfer.finish(transfer.setup == CURLE_OK
        ? curl_easy_perform(transfer.handle.get()) : transfer.setup);
}

} // namespace fluxcrawl
