#pragma once

#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace p2pdc::detail {

// Admits at most `limit` events in each fixed window of steady time. Not
// thread-safe; owners use it from their serialized executor.
class RateWindow {
  public:
    using Clock = std::chrono::steady_clock;
    RateWindow(std::size_t limit, Clock::duration window) : limit_(limit), window_(window) {
        if (limit == 0 || window <= Clock::duration::zero())
            throw std::invalid_argument("invalid rate window");
    }
    [[nodiscard]] bool admit(Clock::time_point now = Clock::now()) {
        if (!started_ || now - start_ >= window_ || now < start_) {
            started_ = true;
            start_ = now;
            count_ = 0;
        }
        if (count_ >= limit_)
            return false;
        ++count_;
        return true;
    }

  private:
    std::size_t limit_;
    Clock::duration window_;
    Clock::time_point start_{};
    std::size_t count_ = 0;
    bool started_ = false;
};

} // namespace p2pdc::detail
