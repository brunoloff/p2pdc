#include "async_signaling.hpp"

#include <exception>
#include <utility>

namespace p2pdc::detail {
namespace {
SignalingFlowCallbacks async_callbacks(SignalingFlowCallbacks callbacks) {
    callbacks.asynchronous_application = true;
    return callbacks;
}
} // namespace

AsyncSignaling::AsyncSignaling(AsyncSignalingBinding binding, ByteView capability,
                               CandidateLimits limits, MemoryBudget budget,
                               std::size_t max_candidate_bytes, std::size_t max_candidates,
                               SignalingFlowCallbacks callbacks)
    : binding_(binding), limits_(limits),
      session_(binding.local, binding.remote, binding.local_box, binding.remote_box,
               binding.attempt, binding.role, capability, 63),
      flow_(session_, limits, std::move(budget), max_candidate_bytes, max_candidates,
            async_callbacks(std::move(callbacks))) {}

FlowResult AsyncSignaling::state() const {
    return closed_ ? FlowResult::Closed : failed_ ? FlowResult::Failed : FlowResult::Accepted;
}
FlowResult AsyncSignaling::track(FlowResult result) {
    if (result == FlowResult::Failed) {
        failed_ = true;
        flow_.close();
    }
    return result;
}
FlowResult AsyncSignaling::local_description(const Identity &identity, ByteView capability,
                                             ByteView installed_sdp, std::uint64_t issued,
                                             std::uint64_t expiry, SignalingPolicy policy) {
    if (state() != FlowResult::Accepted)
        return state();
    if (local_installed_ || installed_sdp.empty())
        return track(FlowResult::Failed);
    const auto kind =
        binding_.role == HandshakeRole::Offerer ? SignalingKind::Offer : SignalingKind::Answer;
    const auto result = flow_.send(identity, binding_.remote_box, capability, kind, installed_sdp,
                                   issued, expiry, policy);
    if (result == FlowResult::Accepted)
        local_installed_ = true;
    return track(result);
}
FlowResult AsyncSignaling::local_candidate(const Identity &identity, ByteView capability,
                                           CandidateView candidate, std::uint64_t issued,
                                           std::uint64_t expiry, SignalingPolicy policy) {
    if (state() != FlowResult::Accepted)
        return state();
    if (!local_installed_)
        return track(FlowResult::Failed);
    try {
        auto encoded = encode_candidate(candidate, limits_);
        if (!std::holds_alternative<Bytes>(encoded))
            return track(FlowResult::Failed);
        return track(flow_.send(identity, binding_.remote_box, capability, SignalingKind::Candidate,
                                std::get<Bytes>(encoded), issued, expiry, policy));
    } catch (const std::exception &) {
        return track(FlowResult::Failed);
    }
}
FlowResult AsyncSignaling::receive(ByteView wire, const RecipientKeyPair &local_box,
                                   ByteView capability, SignalingPolicy policy) {
    if (state() != FlowResult::Accepted)
        return state();
    return track(flow_.receive(wire, local_box, capability, policy));
}
FlowResult AsyncSignaling::description_applied(bool success) {
    if (state() != FlowResult::Accepted)
        return state();
    return track(flow_.description_applied(success));
}
FlowResult AsyncSignaling::candidate_applied(std::uint64_t operation, bool success) {
    if (state() != FlowResult::Accepted)
        return state();
    return track(flow_.candidate_applied(operation, success));
}
std::optional<std::uint64_t> AsyncSignaling::candidate_operation() const {
    return state() == FlowResult::Accepted ? flow_.candidate_operation() : std::nullopt;
}
std::optional<HandshakeContext> AsyncSignaling::handshake_context() const {
    if (state() != FlowResult::Accepted || !local_installed_ || !flow_.description_installed())
        return std::nullopt;
    auto result = session_.handshake_context();
    if (auto *context = std::get_if<HandshakeContext>(&result))
        return *context;
    return std::nullopt;
}
void AsyncSignaling::close() {
    closed_ = true;
    flow_.close();
}
} // namespace p2pdc::detail
