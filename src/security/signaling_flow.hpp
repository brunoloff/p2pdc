#pragma once

#include "signaling_candidate.hpp"
#include "signaling_session.hpp"

#include <functional>

namespace p2pdc::detail {

enum class FlowResult { Accepted, Ignored, Failed, Closed };
struct SignalingFlowCallbacks {
    std::function<bool(Bytes)> submit;
    std::function<bool(ByteView)> apply_description;
    std::function<bool(CandidateView)> apply_candidate;
    std::function<bool(SignalingKind)> apply_control;
    // When true, successful apply callbacks mean started; the owner must call
    // the matching completion method after the transport operation finishes.
    bool asynchronous_application = false;
};

// One fresh, preauthorized signaling session. The session and callback targets
// must outlive this object. Calls and callbacks are serialized; callbacks must
// not throw, reenter, or destroy the flow. submit consumes the encoded record.
// A failed submission or application terminates the flow because the session
// has already committed its sequence/replay state. Untrusted invalid records are
// ignored. At most 63 ordinary records may be sent, starting with the description; this
// provisional bound keeps it in the 64-entry replay window even if DHT delivery
// reorders every record. The receiver's maximum forward jump must be 63.
// Asynchronous apply callbacks must copy their borrowed arguments and complete
// later, without reentering the flow. Only one candidate is applied at a time;
// its operation ID may be read from the candidate callback and must accompany
// the completion, so a stale completion cannot acknowledge the next candidate.
// The owner must also verify local description installation before creating a
// handshake context or allowing application messages.
class SignalingFlow {
  public:
    SignalingFlow(SignalingSession &session, CandidateLimits limits, MemoryBudget budget,
                  std::size_t max_candidate_bytes, std::size_t max_candidates,
                  SignalingFlowCallbacks callbacks);
    [[nodiscard]] FlowResult send(const Identity &identity, const RecipientBoxPublicKey &remote,
                                  ByteView capability, SignalingKind kind, ByteView payload,
                                  std::uint64_t issued, std::uint64_t expiry,
                                  SignalingPolicy policy);
    [[nodiscard]] FlowResult receive(ByteView wire, const RecipientKeyPair &local_box,
                                     ByteView capability, SignalingPolicy policy);
    [[nodiscard]] FlowResult description_applied(bool success);
    [[nodiscard]] FlowResult candidate_applied(std::uint64_t operation, bool success);
    [[nodiscard]] std::optional<std::uint64_t> candidate_operation() const;
    [[nodiscard]] bool description_installed() const;
    [[nodiscard]] bool failed() const;
    void close();

  private:
    [[nodiscard]] FlowResult send_checked(const Identity &identity,
                                          const RecipientBoxPublicKey &remote, ByteView capability,
                                          SignalingKind kind, ByteView payload,
                                          std::uint64_t issued, std::uint64_t expiry,
                                          SignalingPolicy policy);
    [[nodiscard]] FlowResult receive_checked(ByteView wire, const RecipientKeyPair &local_box,
                                             ByteView capability, SignalingPolicy policy);
    [[nodiscard]] FlowResult apply(SignalingEnvelope envelope);
    [[nodiscard]] bool drain_candidates();
    [[nodiscard]] FlowResult fail();
    SignalingSession &session_;
    CandidateBuffer candidates_;
    SignalingFlowCallbacks callbacks_;
    std::size_t sent_ = 0;
    bool failed_ = false;
    bool closed_ = false;
    bool description_pending_ = false;
    bool description_installed_ = false;
    bool candidate_pending_ = false;
    std::uint64_t candidate_operation_ = 0;
};

} // namespace p2pdc::detail
