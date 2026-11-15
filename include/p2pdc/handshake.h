#pragma once

#include "p2pdc/signaling.h"

namespace p2pdc {

using HandshakeHash = std::array<std::byte, 32>;
using HandshakeNonce = std::array<std::byte, 32>;
enum class HandshakeRole : std::uint8_t { Offerer = 1, Answerer = 2 };
enum class HandshakeKind : std::uint8_t { Hello = 1, Confirm = 2 };
enum class HandshakeError { InvalidContext, InvalidRecord, WrongIdentity, WrongRole, InvalidState };
template <class T> using HandshakeResult = std::variant<T, HandshakeError, CryptoError>;

struct HandshakeContext {
    SigningPublicKey offerer_key;
    SigningPublicKey answerer_key;
    SignalingInstanceId offerer_instance;
    SignalingInstanceId answerer_instance;
    AttemptId attempt;
    HandshakeHash group;
    HandshakeHash offer_hash;
    HandshakeHash answer_hash;
};

struct HandshakeRecord {
    HandshakeKind kind;
    HandshakeRole role;
    std::uint32_t max_message_size;
    HandshakeNonce nonce;
    HandshakeNonce peer_nonce;
};

inline constexpr std::size_t handshake_record_size = 139;

// Context must be constructed from authorized identities and the exact signed
// SDP bytes subsequently installed into WebRTC. Attempt IDs must be fresh and
// unpredictable. These helpers do not verify SDP, authorization, or timeouts.
[[nodiscard]] CryptoResult<HandshakeHash> handshake_group(ByteView capability);
[[nodiscard]] CryptoResult<HandshakeHash> handshake_description_hash(ByteView sdp,
                                                                     std::size_t max_size);
[[nodiscard]] CryptoResult<HandshakeNonce> generate_handshake_nonce();
[[nodiscard]] HandshakeResult<Bytes> encode_handshake(const Identity &identity,
                                                      const HandshakeContext &context,
                                                      const HandshakeRecord &record);
[[nodiscard]] HandshakeResult<HandshakeRecord>
decode_handshake(ByteView wire, const HandshakeContext &context, HandshakeRole expected_role);

} // namespace p2pdc
