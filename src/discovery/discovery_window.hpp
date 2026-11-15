#pragma once

#include "p2pdc/discovery_key.h"

#include <cstdint>
#include <variant>

namespace p2pdc::detail {
inline constexpr std::uint64_t discovery_window_seconds = 120;
inline constexpr std::uint64_t max_discovery_windows = 8;

struct DiscoveryWindowRange {
    std::uint64_t first;
    std::uint64_t last;
    bool operator==(const DiscoveryWindowRange &) const = default;
};
enum class DiscoveryWindowError { InvalidLifetime, TooManyWindows };

// These keys are a new protocol namespace and are not compatible with v1 keys.
[[nodiscard]] CryptoResult<DiscoveryKey> derive_discovery_window_key(ByteView capability,
                                                                     std::uint64_t window);
[[nodiscard]] std::uint64_t discovery_window_for(std::uint64_t time);
// Inclusive range covering every issue time accepted by the time policy.
[[nodiscard]] std::variant<DiscoveryWindowRange, DiscoveryWindowError>
discovery_window_range(std::uint64_t now, std::uint64_t max_lifetime, std::uint64_t clock_skew);
} // namespace p2pdc::detail
