#pragma once

#include "p2pdc/security.h"

namespace p2pdc::detail {

// Raw Ed25519 over a protocol signature input. Only library protocol code may
// use these; every input must begin with its own protocol signature domain.
// Applications use Identity::sign and verify_signature, which add
// application_signature_domain.
struct ProtocolSigning {
    [[nodiscard]] static CryptoResult<Signature> sign(const Identity &identity, ByteView message);
};
[[nodiscard]] CryptoStatus verify_protocol_signature(ByteView public_key, ByteView message,
                                                     ByteView signature);

} // namespace p2pdc::detail
