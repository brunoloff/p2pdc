#pragma once

#include <atomic>
#include <cstdint>

namespace p2pdc::detail {

// Backward wall-clock steps up to this many seconds (the default clock skew)
// are absorbed by holding time at its last value, so an NTP step does not stop
// discovery, leases or listeners. A larger step is adopted as the new time:
// running components still report a clock error, and recreated ones work.
inline constexpr std::uint64_t clock_regression_tolerance = 30;

// UTC seconds from the system clock; throws std::runtime_error before 1970.
[[nodiscard]] std::uint64_t utc_seconds();

// Applies the regression policy to now, given the last value returned.
[[nodiscard]] std::uint64_t forward_seconds(std::atomic<std::uint64_t> &last, std::uint64_t now,
                                            std::uint64_t tolerance);

// Process-wide utc_seconds() with the regression policy applied.
[[nodiscard]] std::uint64_t forward_utc_seconds();

} // namespace p2pdc::detail
