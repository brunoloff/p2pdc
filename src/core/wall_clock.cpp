#include "wall_clock.hpp"

#include <chrono>
#include <stdexcept>

namespace p2pdc::detail {

std::uint64_t utc_seconds() {
    const auto value = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (value < 0)
        throw std::runtime_error("system clock is before the Unix epoch");
    return static_cast<std::uint64_t>(value);
}

std::uint64_t forward_seconds(std::atomic<std::uint64_t> &last, std::uint64_t now,
                              std::uint64_t tolerance) {
    auto previous = last.load();
    for (;;) {
        if (now < previous && previous - now <= tolerance)
            return previous;
        if (last.compare_exchange_weak(previous, now))
            return now;
    }
}

std::uint64_t forward_utc_seconds() {
    static std::atomic<std::uint64_t> last{0};
    return forward_seconds(last, utc_seconds(), clock_regression_tolerance);
}

} // namespace p2pdc::detail
