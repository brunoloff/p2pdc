#pragma once

#include "p2pdc/security.h"

namespace p2pdc {

using DiscoveryCapability = std::array<std::byte, capability_size>;
using DiscoveryKey = std::array<std::byte, 20>;

// The capability is secret bearer material; callers must protect and wipe copies.
[[nodiscard]] CryptoResult<DiscoveryCapability> generate_discovery_capability();
// Domain-separated keyed BLAKE2b; fixed namespace for this protocol version.
[[nodiscard]] CryptoResult<DiscoveryKey> derive_discovery_key(ByteView capability);

} // namespace p2pdc
