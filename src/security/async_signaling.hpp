#pragma once

#include "signaling_flow.hpp"

namespace p2pdc::detail {

struct AsyncSignalingBinding {
    SignalingRecipient local;
    SignalingRecipient remote;
    RecipientBoxPublicKey local_box;
    RecipientBoxPublicKey remote_box;
    AttemptId attempt;
    HandshakeRole role;
};

// One preauthorized attempt, serialized and non-reentrant. Call local_description
// only with the exact bytes successfully installed by this transport. Remote
// callbacks borrow their input: copy it before returning. Their completion must
// be reported later, not during a callback. Fingerprint verification, deadlines,
// and lifetime of callback targets are enforced by the owner. Context is never
// returned until both signed descriptions have been installed.
class AsyncSignaling {
  public:
    AsyncSignaling(AsyncSignalingBinding binding, ByteView capability, CandidateLimits limits,
                   MemoryBudget budget, std::size_t max_candidate_bytes, std::size_t max_candidates,
                   SignalingFlowCallbacks callbacks);
    AsyncSignaling(const AsyncSignaling &) = delete;
    AsyncSignaling &operator=(const AsyncSignaling &) = delete;
    [[nodiscard]] FlowResult local_description(const Identity &identity, ByteView capability,
                                               ByteView installed_sdp, std::uint64_t issued,
                                               std::uint64_t expiry, SignalingPolicy policy);
    [[nodiscard]] FlowResult local_candidate(const Identity &identity, ByteView capability,
                                             CandidateView candidate, std::uint64_t issued,
                                             std::uint64_t expiry, SignalingPolicy policy);
    [[nodiscard]] FlowResult receive(ByteView wire, const RecipientKeyPair &local_box,
                                     ByteView capability, SignalingPolicy policy);
    [[nodiscard]] FlowResult description_applied(bool success);
    [[nodiscard]] FlowResult candidate_applied(std::uint64_t operation, bool success);
    [[nodiscard]] std::optional<std::uint64_t> candidate_operation() const;
    [[nodiscard]] std::optional<HandshakeContext> handshake_context() const;
    void close();

  private:
    [[nodiscard]] FlowResult state() const;
    [[nodiscard]] FlowResult track(FlowResult result);
    AsyncSignalingBinding binding_;
    CandidateLimits limits_;
    SignalingSession session_;
    SignalingFlow flow_;
    bool local_installed_ = false;
    bool failed_ = false;
    bool closed_ = false;
};

} // namespace p2pdc::detail
