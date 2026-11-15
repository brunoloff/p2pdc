#pragma once

#include "p2pdc/recipient_crypto.h"

#include <string_view>

namespace p2pdc {

using SignalingInstanceId = std::array<std::byte, 16>;
using AttemptId = std::array<std::byte, 16>;
enum class SignalingKind : std::uint8_t {
    Unknown = 0,
    Offer = 1,
    Answer,
    Candidate,
    Cancel,
    Reject
};

struct SignalingRecipient {
    SigningPublicKey signing_key;
    SignalingInstanceId instance;
    bool operator==(const SignalingRecipient &) const = default;
};

struct SignalingFields {
    SignalingKind kind;
    SignalingInstanceId sender_instance;
    SignalingRecipient recipient;
    AttemptId attempt_id;
    std::uint64_t sequence;
    std::uint64_t issued_at;
    std::uint64_t expires_at;
    Bytes payload;
    bool operator==(const SignalingFields &) const = default;
};

struct SignalingEnvelope {
    SigningPublicKey sender_signing_key;
    SignalingFields fields;
    bool operator==(const SignalingEnvelope &) const = default;
};

struct SignalingPolicy {
    std::uint64_t now;
    std::uint64_t max_lifetime;
    std::uint64_t clock_skew;
    std::size_t max_payload_size;
};

enum class SignalingError {
    InvalidLength,
    InvalidMagic,
    UnsupportedVersion,
    InvalidKind,
    InvalidPolicy,
    TooLarge,
    InvalidLifetime,
    NotYetValid,
    Expired,
    RecipientMismatch
};
template <class T> using SignalingResult = std::variant<T, SignalingError, CryptoError>;

inline constexpr std::size_t signaling_header_size = 5;
inline constexpr std::size_t signaling_body_header_size = 146;
inline constexpr std::size_t signaling_signed_overhead =
    signaling_body_header_size + signature_size;
inline constexpr std::size_t signaling_wire_overhead =
    signaling_header_size + recipient_box_overhead + signaling_signed_overhead;
inline constexpr std::string_view signaling_signature_domain = "p2pdc/signaling/signature/v1";
inline constexpr std::string_view signaling_group_domain = "p2pdc/signaling/group-binding/v1";

// Provisional format: outer "PDSG", u8 version=1, libsodium sealed box. Its
// plaintext is canonical body || signature[64]. Body: repeated outer header,
// u8 kind, sender signing key[32], recipient signing key[32], sender instance[16],
// recipient instance[16], attempt[16], sequence/issued/expiry (big-endian u64s),
// payload length (big-endian u32), payload. No padding or trailing bytes.
// Signature input: signature_domain || group_context[32] || body, where context
// is BLAKE2b-256(key=capability, message=group_domain), recomputed, not transmitted.
// Sender signing key is derived from Identity; recipient encryption key must come
// from its verified advertisement. Payload is opaque (including complete SDP).
// max_lifetime must be nonzero; 0 < expiry-issued <= max_lifetime. Future issue
// may differ by at most clock_skew. Expiry is exclusive, extended by clock_skew.
// Caller generates unpredictable attempt/instance IDs and enforces sequence
// monotonicity, replay/dedup, expected-sender authorization and attempt binding.
// Valid signatures identify keys, not application authorization. Container
// allocation failures can throw; per-record allocations are bounded by policy.
[[nodiscard]] SignalingResult<Bytes>
encode_signaling(const Identity &sender, const SignalingFields &fields,
                 const RecipientBoxPublicKey &recipient_encryption_key, ByteView capability,
                 SignalingPolicy policy);
[[nodiscard]] SignalingResult<SignalingEnvelope>
decode_signaling(ByteView wire, const RecipientKeyPair &recipient_key,
                 const SignalingRecipient &expected_recipient, ByteView capability,
                 SignalingPolicy policy);

} // namespace p2pdc
