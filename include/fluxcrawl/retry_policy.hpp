#pragma once
#include <fluxcrawl/http_response.hpp>
#include <chrono>
#include <cstddef>

namespace fluxcrawl {
struct RetryPolicy {
    std::size_t max_retries = 0; // Initial attempt plus this many retries.
    std::chrono::milliseconds initial_backoff{250};
    std::chrono::milliseconds max_backoff{5000};
    double multiplier = 2.0;
    bool jitter = true;
    void validate() const;
    // retry_number is one-based; sample is a uniform value in [0,1].
    [[nodiscard]] std::chrono::milliseconds delay(std::size_t retry_number, double sample = 1) const;
};
[[nodiscard]] bool retryable_status(long status) noexcept;
[[nodiscard]] bool retryable_error(const HttpError& error) noexcept;
// Integer delta-seconds only; malformed/date/overflow values are ignored.
[[nodiscard]] std::optional<std::chrono::seconds> parse_retry_after(std::string_view value) noexcept;
} // namespace fluxcrawl
