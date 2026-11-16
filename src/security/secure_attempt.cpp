#include "secure_attempt.hpp"

#include <sodium.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
AsyncSignalingBinding checked_binding(AsyncSignalingBinding binding,
                                      const std::shared_ptr<const Identity> &identity,
                                      const RecipientKeyPair &box) {
    if (!identity || binding.local.signing_key != identity->public_key() ||
        binding.local_box != box.public_key())
        throw std::invalid_argument("local identity does not match signaling binding");
    return binding;
}
AttemptFailure map_failure(SecureFailure value) {
    switch (value) {
    case SecureFailure::Protocol:
        return AttemptFailure::Protocol;
    case SecureFailure::Transport:
        return AttemptFailure::Transport;
    case SecureFailure::Resource:
        return AttemptFailure::Resource;
    case SecureFailure::Timeout:
        return AttemptFailure::Timeout;
    }
    return AttemptFailure::Resource;
}
} // namespace

SecureAttempt::Secret::Secret(ByteView value) {
    if (value.size() != capability_size)
        throw std::invalid_argument("invalid capability length");
    std::copy(value.begin(), value.end(), bytes.begin());
}
SecureAttempt::Secret::~Secret() { sodium_memzero(bytes.data(), bytes.size()); }

SecureAttempt::SecureAttempt(AsyncSignalingBinding binding, ByteView capability,
                             std::shared_ptr<const Identity> identity,
                             const RecipientKeyPair &local_box, CandidateLimits candidate_limits,
                             MemoryBudget budget, ReliableChannelLimits channel_limits,
                             std::uint32_t receive_limit, ReliableSink &sink, Executor executor,
                             SecureAttemptCallbacks callbacks)
    : identity_(std::move(identity)), local_box_(local_box), capability_(capability),
      callbacks_(std::move(callbacks)),
      signaling_(checked_binding(binding, identity_, local_box), capability_.bytes,
                 candidate_limits, budget, 65536, 63, signaling_callbacks()),
      channel_(signaling_, identity_, binding.role, receive_limit, channel_limits,
               std::move(budget), sink, std::move(executor), channel_callbacks()) {
    if (!callbacks_.on_ready || !callbacks_.on_message || !callbacks_.on_writable ||
        !callbacks_.on_failure)
        throw std::invalid_argument("secure attempt requires callbacks");
}
SecureAttempt::~SecureAttempt() { close(); }
SignalingFlowCallbacks SecureAttempt::signaling_callbacks() {
    auto callbacks = callbacks_.signaling;
    if (!callbacks.apply_control)
        throw std::invalid_argument("missing signaling control callback");
    callbacks.apply_control = [this](SignalingKind kind) {
        const bool applied = callbacks_.signaling.apply_control(kind);
        if (kind == SignalingKind::Reject)
            signaling_failure_ = AttemptFailure::Rejected;
        else if (kind == SignalingKind::Cancel)
            signaling_failure_ = AttemptFailure::Canceled;
        return applied && kind != SignalingKind::Cancel && kind != SignalingKind::Reject;
    };
    return callbacks;
}
SecureChannelCallbacks SecureAttempt::channel_callbacks() {
    return {[this](std::uint32_t limit) {
                if (!stopped_)
                    callbacks_.on_ready(limit);
            },
            [this](Bytes message) {
                if (!stopped_)
                    callbacks_.on_message(std::move(message));
            },
            [this] {
                if (!stopped_)
                    callbacks_.on_writable();
            },
            [this](SecureFailure reason) { fail(map_failure(reason)); }};
}
FlowResult SecureAttempt::track(FlowResult result) {
    if (result == FlowResult::Failed) {
        fail(signaling_failure_);
        return result;
    }
    if (result == FlowResult::Accepted)
        maybe_start();
    return stopped_ ? FlowResult::Failed : result;
}
void SecureAttempt::maybe_start() {
    if (!stopped_ && !started_ && signaling_.handshake_context())
        started_ = channel_.start();
}
FlowResult SecureAttempt::local_description(ByteView sdp, std::uint64_t issued,
                                            std::uint64_t expiry, SignalingPolicy policy) {
    if (stopped_)
        return FlowResult::Closed;
    return track(
        signaling_.local_description(*identity_, capability_.bytes, sdp, issued, expiry, policy));
}
FlowResult SecureAttempt::local_candidate(CandidateView candidate, std::uint64_t issued,
                                          std::uint64_t expiry, SignalingPolicy policy) {
    if (stopped_)
        return FlowResult::Closed;
    return track(signaling_.local_candidate(*identity_, capability_.bytes, candidate, issued,
                                            expiry, policy));
}
FlowResult SecureAttempt::receive_signaling(ByteView wire, SignalingPolicy policy) {
    if (stopped_)
        return FlowResult::Closed;
    return track(signaling_.receive(wire, local_box_, capability_.bytes, policy));
}
FlowResult SecureAttempt::description_applied(bool success) {
    if (stopped_)
        return FlowResult::Closed;
    return track(signaling_.description_applied(success));
}
FlowResult SecureAttempt::candidate_applied(std::uint64_t operation, bool success) {
    if (stopped_)
        return FlowResult::Closed;
    return track(signaling_.candidate_applied(operation, success));
}
std::optional<std::uint64_t> SecureAttempt::candidate_operation() const {
    return stopped_ ? std::nullopt : signaling_.candidate_operation();
}
ChannelSend SecureAttempt::send(ByteView message) {
    return stopped_ ? ChannelSend::Closed : channel_.send(message);
}
void SecureAttempt::set_transport_ready(bool ready) {
    if (!stopped_)
        channel_.set_transport_ready(ready);
}
void SecureAttempt::on_frame(ByteView frame) {
    if (!stopped_)
        channel_.on_frame(frame);
}
void SecureAttempt::wake() {
    if (!stopped_)
        channel_.wake();
}
void SecureAttempt::timeout() {
    if (!stopped_)
        channel_.timeout();
}
bool SecureAttempt::ready() const { return !stopped_ && channel_.ready(); }
void SecureAttempt::fail(AttemptFailure reason) {
    if (stopped_)
        return;
    stopped_ = true;
    signaling_.close();
    channel_.close();
    callbacks_.on_failure(reason);
}
void SecureAttempt::close() {
    if (stopped_)
        return;
    stopped_ = true;
    signaling_.close();
    channel_.close();
}
} // namespace p2pdc::detail
