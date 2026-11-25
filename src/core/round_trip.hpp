#pragma once

#include <chrono>
#include <optional>

namespace p2pdc::detail {

// Smooths the round-trip times WebRTC reports for a connection's selected path, like TCP's
// smoothed RTT (RFC 6298) but with a gain of 1/4, since samples come only every couple of
// seconds. The first sample is taken as it is.
class RoundTripSmoother {
  public:
    static constexpr std::chrono::milliseconds sample_interval{2000};
    static constexpr std::chrono::milliseconds max_sample{60000};

    // True once per sample_interval: when a new sample should be taken at now.
    [[nodiscard]] bool due(std::chrono::steady_clock::time_point now);
    // Adds a sample; false, changing nothing, for a negative or implausibly long one.
    bool sample(std::chrono::microseconds value);
    // Forgets the path, as when it changes or the connection stops.
    void reset();
    [[nodiscard]] std::optional<std::chrono::milliseconds> value() const;

  private:
    std::optional<double> smoothed_us_;
    std::optional<std::chrono::steady_clock::time_point> last_due_;
};

} // namespace p2pdc::detail
