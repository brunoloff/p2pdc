#include "discovery_window.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <limits>

namespace p2pdc::detail {
CryptoResult<DiscoveryKey> derive_discovery_window_key(ByteView capability, std::uint64_t window) {
    if (capability.size() != capability_size)
        return CryptoError::InvalidLength;
    if (sodium_init() < 0)
        return CryptoError::InitializationFailed;
    constexpr unsigned char domain[] = "p2pdc/discovery/window-key/v2";
    std::array<unsigned char, sizeof(domain) - 1 + 8> input{};
    std::copy_n(domain, sizeof(domain) - 1, input.begin());
    for (std::size_t i = 0; i < 8; ++i)
        input[sizeof(domain) - 1 + i] = static_cast<unsigned char>(window >> (56 - i * 8));
    DiscoveryKey key{};
    if (crypto_generichash(reinterpret_cast<unsigned char *>(key.data()), key.size(), input.data(),
                           input.size(), reinterpret_cast<const unsigned char *>(capability.data()),
                           capability.size()) != 0)
        return CryptoError::OperationFailed;
    return key;
}

std::uint64_t discovery_window_for(std::uint64_t time) { return time / discovery_window_seconds; }

std::variant<DiscoveryWindowRange, DiscoveryWindowError>
discovery_window_range(std::uint64_t now, std::uint64_t max_lifetime, std::uint64_t clock_skew) {
    if (!max_lifetime)
        return DiscoveryWindowError::InvalidLifetime;
    auto lower = now > max_lifetime ? now - max_lifetime : 0;
    lower = lower > clock_skew ? lower - clock_skew : 0;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto upper = clock_skew > maximum - now ? maximum : now + clock_skew;
    const DiscoveryWindowRange range{discovery_window_for(lower), discovery_window_for(upper)};
    if (range.last - range.first >= max_discovery_windows)
        return DiscoveryWindowError::TooManyWindows;
    return range;
}
} // namespace p2pdc::detail
