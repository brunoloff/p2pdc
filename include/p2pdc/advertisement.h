#pragma once

#include "p2pdc/security.h"

#include <string_view>

namespace p2pdc {

using RecipientPublicKey = std::array<std::byte, 32>; // X25519 public key, caller supplied.
using InstanceId = std::array<std::byte, 16>;
using SignalingInboxKey = std::array<std::byte, 20>;

struct AdvertisementFields {
    RecipientPublicKey recipient_public_key;
    InstanceId instance_id;
    SignalingInboxKey signaling_inbox;
    std::uint64_t sequence;
    std::uint64_t features;
    std::uint64_t issued_at; // UTC seconds since Unix epoch.
    std::uint64_t expires_at;
    bool operator==(const AdvertisementFields &) const = default;
};

struct Advertisement {
    SigningPublicKey signing_public_key;
    AdvertisementFields fields;
    bool operator==(const Advertisement &) const = default;
};

struct AdvertisementPolicy {
    std::uint64_t now;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
};

enum class AdvertisementError {
    InvalidLength,
    InvalidMagic,
    UnsupportedVersion,
    InvalidPolicy,
    InvalidLifetime,
    NotYetValid,
    Expired
};

template <class T> using AdvertisementResult = std::variant<T, AdvertisementError, CryptoError>;

inline constexpr std::size_t advertisement_body_size = 133;
inline constexpr std::size_t advertisement_plaintext_size =
    advertisement_body_size + signature_size;
inline constexpr std::size_t advertisement_header_size = 5;
inline constexpr std::size_t advertisement_wire_size =
    advertisement_header_size + encryption_nonce_size + advertisement_plaintext_size +
    encryption_tag_size;
inline constexpr std::string_view advertisement_signature_domain =
    "p2pdc/advertisement/signature/v1";
inline constexpr std::string_view advertisement_aead_domain = "p2pdc/advertisement/aead/v1";
inline constexpr std::string_view advertisement_group_domain =
    "p2pdc/advertisement/group-binding/v1";

// Provisional v1 format, not a frozen protocol: outer "PDAD", u8 version=1,
// nonce[24], AEAD ciphertext. Plaintext: u8 version=1, signing key[32],
// recipient key[32], instance[16], inbox[20], sequence/features/issued/expiry
// (four big-endian u64s), Ed25519 signature[64]. No padding or trailing bytes.
// Group context: BLAKE2b-256(key=capability, message=group_domain).
// Signature input: signature_domain || group_context[32] || body.
// Group context is recomputed, not transmitted. AAD: aead_domain || outer header.
// Times require 0 < expiry-issued <= max_lifetime; max_lifetime must be nonzero.
// Issue may be at most clock_skew in the future. Expiry is exclusive, extended
// by clock_skew, using mathematical arithmetic without integer wraparound.
// Verified identity is not authorization. Replay/dedup, supported feature policy,
// recipient-key usability, instance generation and PeerId derivation belong above.
// Container allocation failures may throw as in the security primitives.
[[nodiscard]] AdvertisementResult<Bytes> encode_advertisement(const Identity &identity,
                                                              const AdvertisementFields &fields,
                                                              ByteView capability,
                                                              AdvertisementPolicy policy);
[[nodiscard]] AdvertisementResult<Advertisement>
decode_advertisement(ByteView wire, ByteView capability, AdvertisementPolicy policy);

} // namespace p2pdc
