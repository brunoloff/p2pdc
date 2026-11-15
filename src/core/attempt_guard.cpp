#include "p2pdc/attempt_guard.h"

#include <stdexcept>

namespace p2pdc {

AttemptGuard::AttemptGuard(SigningPublicKey sender, SignalingInstanceId instance, AttemptId attempt,
                           std::uint8_t max_forward_jump)
    : sender_(sender), instance_(instance), attempt_(attempt), max_forward_jump_(max_forward_jump) {
    if (max_forward_jump == 0 || max_forward_jump > 64) {
        throw std::invalid_argument("maximum sequence jump must be between 1 and 64");
    }
}

AttemptDecision AttemptGuard::accept(const SignalingEnvelope &envelope) {
    if (envelope.sender_signing_key != sender_) {
        return AttemptDecision::WrongSender;
    }
    if (envelope.fields.sender_instance != instance_) {
        return AttemptDecision::WrongInstance;
    }
    if (envelope.fields.attempt_id != attempt_) {
        return AttemptDecision::WrongAttempt;
    }
    return accept_sequence(envelope.fields.sequence);
}

AttemptDecision AttemptGuard::accept_sequence(std::uint64_t sequence) {
    if (!highest_) {
        if (sequence > max_forward_jump_) {
            return AttemptDecision::TooFarAhead;
        }
        highest_ = sequence;
        seen_ = 1;
        return AttemptDecision::Accepted;
    }
    if (sequence > *highest_) {
        const auto jump = sequence - *highest_;
        if (jump > max_forward_jump_) {
            return AttemptDecision::TooFarAhead;
        }
        seen_ = jump == 64 ? 1 : (seen_ << jump) | 1;
        highest_ = sequence;
        return AttemptDecision::Accepted;
    }
    const auto distance = *highest_ - sequence;
    if (distance >= 64) {
        return AttemptDecision::TooOld;
    }
    const auto bit = std::uint64_t{1} << distance;
    if ((seen_ & bit) != 0) {
        return AttemptDecision::Duplicate;
    }
    seen_ |= bit;
    return AttemptDecision::Accepted;
}

} // namespace p2pdc
