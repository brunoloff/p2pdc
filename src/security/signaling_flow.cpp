#include "signaling_flow.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
bool description(SignalingKind kind) {
    return kind == SignalingKind::Offer || kind == SignalingKind::Answer;
}
} // namespace

SignalingFlow::SignalingFlow(SignalingSession &session, CandidateLimits limits, MemoryBudget budget,
                             std::size_t max_candidate_bytes, std::size_t max_candidates,
                             SignalingFlowCallbacks callbacks)
    : session_(session),
      candidates_(limits, std::move(budget), max_candidate_bytes, max_candidates),
      callbacks_(std::move(callbacks)) {
    if (!callbacks_.submit || !callbacks_.apply_description || !callbacks_.apply_candidate ||
        !callbacks_.apply_control) {
        throw std::invalid_argument("signaling flow requires callbacks");
    }
}

FlowResult SignalingFlow::send(const Identity &identity, const RecipientBoxPublicKey &remote,
                               ByteView capability, SignalingKind kind, ByteView payload,
                               std::uint64_t issued, std::uint64_t expiry, SignalingPolicy policy) {
    if (closed_) {
        return FlowResult::Closed;
    }
    if (failed_) {
        return FlowResult::Failed;
    }
    try {
        return send_checked(identity, remote, capability, kind, payload, issued, expiry, policy);
    } catch (const std::exception &) {
        return fail();
    }
}

FlowResult SignalingFlow::send_checked(const Identity &identity,
                                       const RecipientBoxPublicKey &remote, ByteView capability,
                                       SignalingKind kind, ByteView payload, std::uint64_t issued,
                                       std::uint64_t expiry, SignalingPolicy policy) {
    if (sent_ >= 63 || (sent_ == 0 && !description(kind))) {
        return fail();
    }
    auto encoded =
        session_.encode(identity, remote, capability, kind, payload, issued, expiry, policy);
    if (!std::holds_alternative<Bytes>(encoded)) {
        return fail();
    }
    ++sent_;
    if (!callbacks_.submit(std::move(std::get<Bytes>(encoded)))) {
        return fail();
    }
    return FlowResult::Accepted;
}

FlowResult SignalingFlow::receive(ByteView wire, const RecipientKeyPair &local_box,
                                  ByteView capability, SignalingPolicy policy) {
    if (closed_) {
        return FlowResult::Closed;
    }
    if (failed_) {
        return FlowResult::Failed;
    }
    try {
        return receive_checked(wire, local_box, capability, policy);
    } catch (const std::exception &) {
        return fail();
    }
}

FlowResult SignalingFlow::receive_checked(ByteView wire, const RecipientKeyPair &local_box,
                                          ByteView capability, SignalingPolicy policy) {
    auto verified = session_.verify(wire, local_box, capability, policy);
    if (auto *envelope = std::get_if<SignalingEnvelope>(&verified)) {
        return apply(std::move(*envelope));
    }
    if (auto *error = std::get_if<SessionError>(&verified);
        error &&
        (*error == SessionError::InvalidBinding || *error == SessionError::WrongCapability)) {
        return fail();
    }
    if (auto *error = std::get_if<SignalingError>(&verified);
        error && *error == SignalingError::InvalidPolicy) {
        return fail();
    }
    if (auto *error = std::get_if<CryptoError>(&verified);
        error &&
        (*error == CryptoError::InitializationFailed || *error == CryptoError::AllocationFailed ||
         *error == CryptoError::InvalidIdentity || *error == CryptoError::OperationFailed)) {
        return fail();
    }
    return FlowResult::Ignored;
}

FlowResult SignalingFlow::apply(SignalingEnvelope envelope) {
    const auto kind = envelope.fields.kind;
    if (description(kind)) {
        description_pending_ = true;
        if (!callbacks_.apply_description(envelope.fields.payload)) {
            return fail();
        }
        if (!callbacks_.asynchronous_application)
            return description_applied(true);
    } else if (kind == SignalingKind::Candidate) {
        if (!std::holds_alternative<std::monostate>(candidates_.push(envelope.fields.payload)) ||
            !drain_candidates()) {
            return fail();
        }
    } else if (!callbacks_.apply_control(kind)) {
        return fail();
    }
    return FlowResult::Accepted;
}

bool SignalingFlow::drain_candidates() {
    if (candidate_pending_)
        return true;
    while (auto candidate = candidates_.pop()) {
        if (candidate_operation_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++candidate_operation_;
        candidate_pending_ = true;
        if (!callbacks_.apply_candidate({candidate->candidate, candidate->mid})) {
            return false;
        }
        if (callbacks_.asynchronous_application)
            return true;
        candidate_pending_ = false;
    }
    return true;
}

FlowResult SignalingFlow::description_applied(bool success) {
    if (closed_)
        return FlowResult::Closed;
    if (failed_)
        return FlowResult::Failed;
    if (!description_pending_ || !success)
        return fail();
    description_pending_ = false;
    try {
        if (!candidates_.mark_description_applied())
            return fail();
        description_installed_ = true;
        return drain_candidates() ? FlowResult::Accepted : fail();
    } catch (const std::exception &) {
        return fail();
    }
}

FlowResult SignalingFlow::candidate_applied(std::uint64_t operation, bool success) {
    if (closed_)
        return FlowResult::Closed;
    if (failed_)
        return FlowResult::Failed;
    if (!candidate_pending_ || operation != candidate_operation_ || !success)
        return fail();
    candidate_pending_ = false;
    try {
        return drain_candidates() ? FlowResult::Accepted : fail();
    } catch (const std::exception &) {
        return fail();
    }
}

std::optional<std::uint64_t> SignalingFlow::candidate_operation() const {
    if (candidate_pending_ && !closed_ && !failed_)
        return candidate_operation_;
    return std::nullopt;
}

bool SignalingFlow::description_installed() const {
    return description_installed_ && !failed_ && !closed_;
}

FlowResult SignalingFlow::fail() {
    failed_ = true;
    candidates_.close();
    return FlowResult::Failed;
}

bool SignalingFlow::failed() const { return failed_; }
void SignalingFlow::close() {
    closed_ = true;
    candidates_.close();
}

} // namespace p2pdc::detail
