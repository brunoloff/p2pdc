#pragma once

#include "p2pdc/discovery.h"

#include <array>
#include <chrono>
#include <optional>

namespace p2pdc::detail {
// Limits admissions even when leases are immediately destroyed. Rotating keys
// can replenish capacity; fixed-key publications retain their lifetime bound.
class AnnouncementBudget {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t capacity = 16;
    static constexpr auto interval = std::chrono::minutes(5);

    [[nodiscard]] bool admit(Clock::time_point now, DiscoveryProtocol protocol) {
        if ((protocol != DiscoveryProtocol::RotatingKeysV2 &&
             protocol != DiscoveryProtocol::FixedKeyV1) ||
            (last_ && now < *last_))
            return false;
        last_ = now;
        if (protocol == DiscoveryProtocol::FixedKeyV1 && fixed_ >= capacity)
            return false;
        if (count_ == capacity && (attempts_[next_] > Clock::time_point::max() - interval ||
                                   now < attempts_[next_] + interval))
            return false;
        attempts_[next_] = now;
        next_ = (next_ + 1) % capacity;
        if (count_ < capacity)
            ++count_;
        if (protocol == DiscoveryProtocol::FixedKeyV1)
            ++fixed_;
        return true;
    }

  private:
    std::array<Clock::time_point, capacity> attempts_{};
    std::optional<Clock::time_point> last_;
    std::size_t count_ = 0, next_ = 0, fixed_ = 0;
};
} // namespace p2pdc::detail
