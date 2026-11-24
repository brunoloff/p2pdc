#include "round_trip.hpp"

#include <cmath>

namespace p2pdc::detail {

bool RoundTripSmoother::due(std::chrono::steady_clock::time_point now) {
    if (last_due_ && now - *last_due_ < sample_interval) {
        return false;
    }
    last_due_ = now;
    return true;
}

bool RoundTripSmoother::sample(std::chrono::microseconds value) {
    if (value.count() < 0 || value > max_sample) {
        return false;
    }
    const auto sample = static_cast<double>(value.count());
    smoothed_us_ = smoothed_us_ ? *smoothed_us_ + (sample - *smoothed_us_) / 4.0 : sample;
    return true;
}

void RoundTripSmoother::reset() {
    smoothed_us_.reset();
    last_due_.reset();
}

std::optional<std::chrono::milliseconds> RoundTripSmoother::value() const {
    if (!smoothed_us_) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{std::llround(*smoothed_us_ / 1000.0)};
}

} // namespace p2pdc::detail
