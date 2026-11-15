#pragma once

#include "p2pdc/signaling.h"
#include "record_store.hpp"

namespace p2pdc::detail {
inline constexpr std::uint64_t cancel_sequence = 63;

// Best effort: successful submission is not proof of publication or receipt.
[[nodiscard]] bool publish_terminal_signal(const RecordStore &store, ByteView mailbox,
                                           const Identity &identity, SignalingFields fields,
                                           const RecipientBoxPublicKey &recipient,
                                           ByteView capability, SignalingPolicy policy) noexcept;
} // namespace p2pdc::detail
