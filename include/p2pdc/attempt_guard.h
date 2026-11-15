#pragma once

#include "p2pdc/signaling.h"

#include <optional>

namespace p2pdc {

enum class AttemptDecision {
    Accepted,
    WrongSender,
    WrongInstance,
    WrongAttempt,
    Duplicate,
    TooOld,
    TooFarAhead
};

// One guard per authenticated remote instance and connection attempt. Inputs must
// already have passed decode_signaling, including recipient and capability checks.
// Sequence numbers begin at zero; the first arrival may be as far ahead as the
// configured maximum jump. A 64-record window permits reordering. At jump 64,
// sequence zero is already too old if 64 arrives first. Acceptance is not delivery.
// Calls must be serialized by the owning session. This does not persist replay
// state across process restarts or replace candidate buffering/retransmission.
class AttemptGuard {
  public:
    AttemptGuard(SigningPublicKey sender, SignalingInstanceId instance, AttemptId attempt,
                 std::uint8_t max_forward_jump);
    [[nodiscard]] AttemptDecision accept(const SignalingEnvelope &envelope);

  private:
    [[nodiscard]] AttemptDecision accept_sequence(std::uint64_t sequence);
    SigningPublicKey sender_;
    SignalingInstanceId instance_;
    AttemptId attempt_;
    std::uint8_t max_forward_jump_;
    std::optional<std::uint64_t> highest_;
    std::uint64_t seen_ = 0;
};

} // namespace p2pdc
