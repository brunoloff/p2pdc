#pragma once

#include "attempt_binding.hpp"
#include "native_outgoing_config.hpp"

namespace p2pdc::detail {
// Caller must separately authorize the offer and revalidate the discovery
// handles. This adapter does not accept requests or provide replay protection.
// The returned attempt pins the exact signed offer, not a later DHT record.
[[nodiscard]] NativeAttemptConfig make_native_incoming_config(const ConnectionAdmission &admission,
                                                              const IncomingOffer &offer,
                                                              IceConfig ice, MemoryBudget budget,
                                                              std::function<std::uint64_t()> now);
} // namespace p2pdc::detail
