#include "signaling_session.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
bool nonzero(ByteView bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](auto value) { return value != std::byte{}; });
}
HandshakeHash group_from(ByteView capability) {
    auto result = handshake_group(capability);
    if (auto *value = std::get_if<HandshakeHash>(&result)) {
        return *value;
    }
    throw std::invalid_argument("invalid signaling capability or crypto initialization failed");
}
SignalingKind description_kind(HandshakeRole role) {
    return role == HandshakeRole::Offerer ? SignalingKind::Offer : SignalingKind::Answer;
}
bool is_description(SignalingKind kind) {
    return kind == SignalingKind::Offer || kind == SignalingKind::Answer;
}
} // namespace

SignalingSession::SignalingSession(SignalingRecipient local, SignalingRecipient remote,
                                   RecipientBoxPublicKey local_box,
                                   RecipientBoxPublicKey remote_box, AttemptId attempt,
                                   HandshakeRole role, ByteView capability,
                                   std::uint8_t max_forward_jump)
    : local_(local), remote_(remote), local_box_(local_box), remote_box_(remote_box),
      attempt_(attempt), role_(role), group_(group_from(capability)),
      guard_(remote.signing_key, remote.instance, attempt, max_forward_jump) {
    if (!nonzero(local.signing_key) || !nonzero(remote.signing_key) || !nonzero(local_box) ||
        !nonzero(remote_box) || !nonzero(local.instance) || !nonzero(remote.instance) ||
        !nonzero(attempt) || (role != HandshakeRole::Offerer && role != HandshakeRole::Answerer)) {
        throw std::invalid_argument("invalid signaling session binding");
    }
}

bool SignalingSession::capability_matches(ByteView capability) const {
    auto group = handshake_group(capability);
    return std::holds_alternative<HandshakeHash>(group) && std::get<HandshakeHash>(group) == group_;
}

SessionResult<Bytes> SignalingSession::encode(const Identity &identity,
                                              const RecipientBoxPublicKey &remote_box,
                                              ByteView capability, SignalingKind kind,
                                              ByteView payload, std::uint64_t issued,
                                              std::uint64_t expiry, SignalingPolicy policy) {
    if (identity.public_key() != local_.signing_key || remote_box != remote_box_) {
        return SessionError::InvalidBinding;
    }
    if (!capability_matches(capability)) {
        return SessionError::WrongCapability;
    }
    if (sequence_exhausted_) {
        return SessionError::SequenceExhausted;
    }
    if (kind < SignalingKind::Offer || kind > SignalingKind::Reject) {
        return SignalingError::InvalidKind;
    }
    if (policy.max_lifetime == 0 ||
        policy.max_payload_size > std::numeric_limits<std::uint32_t>::max() ||
        policy.max_payload_size >
            std::numeric_limits<std::size_t>::max() - signaling_wire_overhead) {
        return SignalingError::InvalidPolicy;
    }
    if (payload.size() > policy.max_payload_size) {
        return SignalingError::TooLarge;
    }
    if (is_description(kind) && kind != description_kind(role_)) {
        return SessionError::WrongKind;
    }
    if (is_description(kind) && local_sdp_) {
        return SessionError::DuplicateDescription;
    }
    return encode_checked(identity, remote_box, capability, kind, payload, issued, expiry, policy);
}

SessionResult<Bytes> SignalingSession::encode_checked(const Identity &identity,
                                                      const RecipientBoxPublicKey &remote_box,
                                                      ByteView capability, SignalingKind kind,
                                                      ByteView payload, std::uint64_t issued,
                                                      std::uint64_t expiry,
                                                      SignalingPolicy policy) {
    std::optional<HandshakeHash> digest;
    if (is_description(kind)) {
        auto hashed = handshake_description_hash(payload, policy.max_payload_size);
        if (auto *error = std::get_if<CryptoError>(&hashed)) {
            return *error;
        }
        digest = std::get<HandshakeHash>(hashed);
    }
    SignalingFields fields{
        kind,           local_.instance, remote_, attempt_,
        next_sequence_, issued,          expiry,  Bytes(payload.begin(), payload.end())};
    auto encoded = encode_signaling(identity, fields, remote_box, capability, policy);
    if (auto *error = std::get_if<SignalingError>(&encoded)) {
        return *error;
    }
    if (auto *error = std::get_if<CryptoError>(&encoded)) {
        return *error;
    }
    if (digest) {
        local_sdp_ = *digest;
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        sequence_exhausted_ = true;
    } else {
        ++next_sequence_;
    }
    return std::move(std::get<Bytes>(encoded));
}

SessionResult<SignalingEnvelope> SignalingSession::verify(ByteView wire,
                                                          const RecipientKeyPair &local_box,
                                                          ByteView capability,
                                                          SignalingPolicy policy) {
    if (!capability_matches(capability)) {
        return SessionError::WrongCapability;
    }
    if (local_box.public_key() != local_box_) {
        return SessionError::InvalidBinding;
    }
    auto decoded = decode_signaling(wire, local_box, local_, capability, policy);
    if (auto *error = std::get_if<SignalingError>(&decoded)) {
        return *error;
    }
    if (auto *error = std::get_if<CryptoError>(&decoded)) {
        return *error;
    }
    return accept_verified(std::move(std::get<SignalingEnvelope>(decoded)), policy);
}

SessionResult<SignalingEnvelope> SignalingSession::accept_verified(SignalingEnvelope envelope,
                                                                   SignalingPolicy policy) {
    const auto kind = envelope.fields.kind;
    const auto expected =
        role_ == HandshakeRole::Offerer ? SignalingKind::Answer : SignalingKind::Offer;
    if (is_description(kind) && kind != expected) {
        return SessionError::WrongKind;
    }
    auto staged_guard = guard_;
    const auto decision = staged_guard.accept(envelope);
    if (decision != AttemptDecision::Accepted) {
        return decision;
    }
    if (is_description(kind)) {
        if (remote_sdp_) {
            return SessionError::DuplicateDescription;
        }
        auto hashed = handshake_description_hash(envelope.fields.payload, policy.max_payload_size);
        if (auto *error = std::get_if<CryptoError>(&hashed)) {
            return *error;
        }
        remote_sdp_ = std::get<HandshakeHash>(hashed);
    }
    guard_ = staged_guard;
    return envelope;
}

SessionResult<HandshakeContext> SignalingSession::handshake_context() const {
    if (!local_sdp_ || !remote_sdp_) {
        return SessionError::MissingDescription;
    }
    if (role_ == HandshakeRole::Offerer) {
        return HandshakeContext{local_.signing_key, remote_.signing_key,
                                local_.instance,    remote_.instance,
                                attempt_,           group_,
                                *local_sdp_,        *remote_sdp_};
    }
    return HandshakeContext{remote_.signing_key, local_.signing_key, remote_.instance,
                            local_.instance,     attempt_,           group_,
                            *remote_sdp_,        *local_sdp_};
}

} // namespace p2pdc::detail
