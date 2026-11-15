#include "attempt_binding.hpp"
#include "../security/signaling_limits.hpp"
#include "p2pdc/attempt_guard.h"

#include <sodium.h>

#include <algorithm>

namespace p2pdc::detail {
namespace {
bool nonzero(ByteView bytes) {
    return std::any_of(bytes.begin(), bytes.end(),
                       [](std::byte value) { return value != std::byte{}; });
}
void validate_fields(const ConnectionAdmission &value, const MemoryBudget &budget,
                     const std::function<std::uint64_t()> &now) {
#ifndef __EMSCRIPTEN__
    if (!value.runner || !value.runner->isRunning())
        throw std::invalid_argument("connection runner is not running");
#endif
    if (!value.local.identity || !value.local.identity->signing ||
        !value.local.identity->recipient || !value.capability ||
        value.capability->size() != capability_size || !now ||
        budget.limit() < 524288 + signaling_wire_limit || !value.remote_max_lifetime ||
        value.local.fields.recipient_public_key != value.local.identity->recipient->public_key() ||
        !nonzero(value.local.fields.instance_id) || !nonzero(value.local.fields.signaling_inbox) ||
        !nonzero(value.remote.signing_public_key) || !nonzero(value.remote.fields.instance_id) ||
        !nonzero(value.remote.fields.signaling_inbox) ||
        !nonzero(value.remote.fields.recipient_public_key) ||
        (value.local.identity->signing->public_key() == value.remote.signing_public_key &&
         value.local.fields.instance_id == value.remote.fields.instance_id))
        throw std::invalid_argument("invalid connection admission");
}
void validate_times(const ConnectionAdmission &value, std::uint64_t now) {
    if (value.local.fields.issued_at > now || now >= value.local.fields.expires_at ||
        value.remote.fields.expires_at <= value.remote.fields.issued_at ||
        value.remote.fields.expires_at - value.remote.fields.issued_at >
            value.remote_max_lifetime ||
        (value.remote.fields.issued_at > now &&
         value.remote.fields.issued_at - now > value.remote_clock_skew) ||
        (now >= value.remote.fields.expires_at &&
         now - value.remote.fields.expires_at >= value.remote_clock_skew))
        throw std::invalid_argument("connection admission has expired");
}
} // namespace

AsyncSignalingBinding make_attempt_binding(const ConnectionAdmission &value,
                                           const MemoryBudget &budget,
                                           const std::function<std::uint64_t()> &now,
                                           AttemptId attempt, HandshakeRole role) {
    validate_fields(value, budget, now);
    validate_times(value, now());
    if (!nonzero(attempt) || (role != HandshakeRole::Offerer && role != HandshakeRole::Answerer))
        throw std::invalid_argument("invalid attempt binding");
    const auto &local = value.local;
    const auto &remote = value.remote;
    return {{local.identity->signing->public_key(), local.fields.instance_id},
            {remote.signing_public_key, remote.fields.instance_id},
            local.identity->recipient->public_key(),
            remote.fields.recipient_public_key,
            attempt,
            role};
}
AttemptId random_attempt_id() {
    if (sodium_init() < 0)
        throw std::runtime_error("cannot initialize cryptographic randomness");
    AttemptId result{};
    do {
        randombytes_buf(result.data(), result.size());
    } while (!nonzero(result));
    return result;
}
IncomingCryptoFailure::IncomingCryptoFailure(CryptoError error)
    : std::runtime_error("incoming offer cryptographic operation failed"), error_(error) {}
void verify_incoming_offer(const ConnectionAdmission &admission, const IncomingOffer &offer,
                           const AsyncSignalingBinding &binding, SignalingPolicy policy) {
    if (offer.peer != admission.remote || offer.envelope.fields.kind != SignalingKind::Offer ||
        !nonzero(offer.envelope.fields.attempt_id))
        throw IncomingOfferInvalid("incoming offer does not match the admitted peer");
    auto decoded = decode_signaling(offer.wire, *admission.local.identity->recipient, binding.local,
                                    *admission.capability, policy);
    if (auto *error = std::get_if<CryptoError>(&decoded);
        error &&
        (*error == CryptoError::InitializationFailed || *error == CryptoError::AllocationFailed ||
         *error == CryptoError::InvalidIdentity || *error == CryptoError::OperationFailed))
        throw IncomingCryptoFailure(*error);
    auto envelope = std::get_if<SignalingEnvelope>(&decoded);
    if (!envelope || *envelope != offer.envelope ||
        envelope->sender_signing_key != binding.remote.signing_key ||
        envelope->fields.sender_instance != binding.remote.instance)
        throw IncomingOfferInvalid("incoming offer failed verification");
    AttemptGuard guard(binding.remote.signing_key, binding.remote.instance,
                       envelope->fields.attempt_id, 63);
    if (guard.accept(*envelope) != AttemptDecision::Accepted)
        throw IncomingOfferInvalid("incoming offer sequence is invalid");
}
} // namespace p2pdc::detail
