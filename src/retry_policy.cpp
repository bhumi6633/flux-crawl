#include <fluxcrawl/retry_policy.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace fluxcrawl {
void RetryPolicy::validate() const
{
    if (initial_backoff.count() <= 0 || max_backoff < initial_backoff
        || !std::isfinite(multiplier) || multiplier < 1) {
        throw std::invalid_argument("Retry backoff must be positive, cap >= initial, and multiplier finite >= 1");
    }
}
std::chrono::milliseconds RetryPolicy::delay(std::size_t retry_number, double sample) const
{
    validate();
    if (retry_number == 0 || !std::isfinite(sample) || sample < 0 || sample > 1) {
        throw std::invalid_argument("Retry number must be positive and jitter sample in [0,1]");
    }
    const long double initial = static_cast<long double>(initial_backoff.count());
    const long double cap = static_cast<long double>(max_backoff.count());
    long double value = initial;
    if (multiplier > 1 && retry_number > 1) {
        const auto exponent = static_cast<long double>(retry_number - 1);
        const auto cap_exponent = std::log(cap / initial) / std::log(static_cast<long double>(multiplier));
        value = exponent >= cap_exponent ? cap : std::min(cap, initial * std::pow(static_cast<long double>(multiplier), exponent));
    }
    if (jitter) { value *= sample; } // Full jitter, inclusive zero, rounded down to ms.
    if (value >= cap) { return max_backoff; }
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value)};
}
bool retryable_status(long status) noexcept
{
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}
bool retryable_error(const HttpError& error) noexcept
{
    if (error.code != HttpErrorCode::CurlError) { return false; }
    switch (error.curl_code) {
    case CURLE_OPERATION_TIMEDOUT: case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT: case CURLE_RECV_ERROR: case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING: case CURLE_PARTIAL_FILE: return true;
    default: return false;
    }
}
std::optional<std::chrono::seconds> parse_retry_after(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) { return std::nullopt; }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    std::chrono::seconds::rep seconds = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error != std::errc{} || end != value.data() + value.size() || seconds < 0 || value.front() == '-') { return std::nullopt; }
    return std::chrono::seconds{seconds};
}
} // namespace fluxcrawl
