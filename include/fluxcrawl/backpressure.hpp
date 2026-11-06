#pragma once
#include <chrono>
#include <cstddef>
#include <stdexcept>
namespace fluxcrawl {
struct BackpressureOptions {
    // null high disables admission control; active resources reserve retry capacity.
    std::size_t high_watermark = 10000, low_watermark = 7500;
    bool enabled = true;
    void validate() const { if (enabled && (high_watermark == 0 || low_watermark >= high_watermark)) { throw std::invalid_argument("Pending low watermark must be less than positive high watermark"); } }
};
struct QueuePressureMetrics {
    std::size_t current_pending = 0, peak_pending = 0, peak_unfinished = 0, peak_seen = 0;
    std::size_t deferred_urls = 0, peak_deferred = 0, deferred_admissions = 0;
    std::size_t activations = 0, resumptions = 0;
    std::chrono::nanoseconds pressure_time{}, deferred_io_time{};
};
class PressureGate {
public:
    using Clock = std::chrono::steady_clock;
    explicit PressureGate(BackpressureOptions options) : options_{options} { options.validate(); }
    void update(std::size_t unfinished, Clock::time_point now = Clock::now()) {
        if (!options_.enabled) { return; }
        if (!active_ && unfinished >= options_.high_watermark) { active_ = true; since_ = now; ++activations_; }
        else if (active_ && unfinished <= options_.low_watermark) { active_ = false; elapsed_ += now - since_; ++resumptions_; }
    }
    bool admits(std::size_t unfinished) const { return !options_.enabled || (!active_ && unfinished < options_.high_watermark); }
    bool has_activated() const { return activations_ != 0; }
    bool active() const { return active_; }
    void metrics(QueuePressureMetrics& out, Clock::time_point now = Clock::now()) const {
        out.activations = activations_; out.resumptions = resumptions_;
        out.pressure_time = elapsed_ + (active_ ? now - since_ : Clock::duration::zero());
    }
private:
    BackpressureOptions options_;
    bool active_ = false;
    std::size_t activations_ = 0, resumptions_ = 0;
    Clock::time_point since_{};
    Clock::duration elapsed_{};
};
}
