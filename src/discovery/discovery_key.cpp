#include "p2pdc/discovery_key.h"

#include <sodium.h>

namespace p2pdc {

CryptoResult<DiscoveryCapability> generate_discovery_capability() {
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    DiscoveryCapability capability{};
    randombytes_buf(capability.data(), capability.size());
    return capability;
}

CryptoResult<DiscoveryKey> derive_discovery_key(ByteView capability) {
    if (capability.size() != capability_size) {
        return CryptoError::InvalidLength;
    }
    if (sodium_init() < 0) {
        return CryptoError::InitializationFailed;
    }
    constexpr unsigned char domain[] = "p2pdc/discovery/lookup-key/v1";
    DiscoveryKey result{};
    if (crypto_generichash(reinterpret_cast<unsigned char *>(result.data()), result.size(), domain,
                           sizeof(domain) - 1,
                           reinterpret_cast<const unsigned char *>(capability.data()),
                           capability.size()) != 0) {
        return CryptoError::OperationFailed;
    }
    return result;
}

} // namespace p2pdc
