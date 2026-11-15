#pragma once

#include "../security/signaling_flow.hpp"
#include "native_reliable_link.hpp"

namespace p2pdc::detail {

struct NativeSignalBinding {
    SignalingRecipient local;
    SignalingRecipient remote;
    RecipientBoxPublicKey local_box;
    RecipientBoxPublicKey remote_box;
    AttemptId attempt;
    HandshakeRole role;
};

// Internal adapter for one preauthorized attempt. The link and callback
// targets outlive it; callbacks are serialized, nonthrowing and nonreentrant.
// submit must queue bytes for later delivery, not call the remote peer inline.
// Local descriptions and candidates must come from this exact link's callbacks;
// the adapter cannot independently check that they originated from that link.
// Caller supplies matching identity/box keys and enforces deadlines. A Failed
// result requires closing the entire attempt; raw link messages are untrusted
// until the separate signed handshake gate has succeeded.
class NativeSignaling {
  public:
    NativeSignaling(NativeSignalBinding binding, ByteView capability, NativeReliableLink &link,
                    CandidateLimits candidate_limits, MemoryBudget budget,
                    std::size_t max_candidate_bytes, std::size_t max_candidates,
                    std::function<bool(Bytes)> submit, std::function<bool(SignalingKind)> control);
    NativeSignaling(const NativeSignaling &) = delete;
    NativeSignaling &operator=(const NativeSignaling &) = delete;
    [[nodiscard]] FlowResult local_description(const Identity &identity, ByteView capability,
                                               rtc::Description description, std::uint64_t issued,
                                               std::uint64_t expiry, SignalingPolicy policy);
    [[nodiscard]] FlowResult local_candidate(const Identity &identity, ByteView capability,
                                             rtc::Candidate candidate, std::uint64_t issued,
                                             std::uint64_t expiry, SignalingPolicy policy);
    [[nodiscard]] FlowResult receive(ByteView wire, const RecipientKeyPair &local_box,
                                     ByteView capability, SignalingPolicy policy);
    [[nodiscard]] std::optional<HandshakeContext> handshake_context() const;
    void close();

  private:
    [[nodiscard]] bool apply_description(ByteView payload);
    [[nodiscard]] bool apply_candidate(CandidateView candidate);
    [[nodiscard]] FlowResult state() const;
    NativeSignalBinding binding_;
    NativeReliableLink &link_;
    CandidateLimits limits_;
    SignalingSession session_;
    SignalingFlow flow_;
    bool failed_ = false;
    bool closed_ = false;
};

} // namespace p2pdc::detail
