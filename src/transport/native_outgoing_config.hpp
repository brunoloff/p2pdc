#pragma once

#include "../discovery/connection_admission.hpp"
#include "native_attempt.hpp"

namespace p2pdc::detail {

// The admission must be freshly resolved on the Node executor immediately
// before constructing and starting an attempt. This only maps configuration:
// it does not retain permission after Node/handle closure or register cleanup.
// Attempts sharing an inbox must independently reject records bound to other attempts.
// now must be nonblocking, nonthrowing, UTC and must not reenter the owner.
// The verified subscription's clock-skew policy also governs incoming signaling;
// the remote side independently applies its own policy to outgoing records.
[[nodiscard]] NativeAttemptConfig make_native_outgoing_config(const ConnectionAdmission &admission,
                                                              IceConfig ice, MemoryBudget budget,
                                                              std::function<std::uint64_t()> now);

// Shared configuration builder for an already chosen, nonzero attempt ID.
[[nodiscard]] NativeAttemptConfig make_native_attempt_config(const ConnectionAdmission &admission,
                                                             IceConfig ice, MemoryBudget budget,
                                                             std::function<std::uint64_t()> now,
                                                             AttemptId attempt, HandshakeRole role);

} // namespace p2pdc::detail
