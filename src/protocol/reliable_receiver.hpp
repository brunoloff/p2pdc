#pragma once

#include "p2pdc/memory_budget.h"
#include "p2pdc/reassembler.h"

namespace p2pdc::detail {

enum class ReceiveError { InvalidFrame, ResourceLimit, AllocationFailed };

struct ReservedMessage {
    Bytes bytes;
    MemoryReservation reservation;
    std::optional<MemoryReservation> extra;
};

struct ReceiveResult {
    std::optional<ReservedMessage> message;
    std::optional<ReceiveError> error;
    std::optional<FrameError> frame_error;
};

// Serialized caller context. Reservations account for payload vector capacity
// through delivery, excluding incoming frames, backend buffers and allocator overhead.
// If reserve allocates excess vector capacity, it is charged after allocation;
// this does not enforce a portable instantaneous peak-memory bound. Errors reset
// this primitive; the connection must treat framing errors as terminal.
class ReliableReceiver {
  public:
    ReliableReceiver(FrameLimits limits, MemoryBudget budget);
    [[nodiscard]] ReceiveResult push(ByteView frame);
    // Caps the size of the next messages to start (at most the frame limit).
    // A message announcing a larger total size fails with ResourceLimit before
    // any memory is reserved, so an unauthenticated peer cannot make the
    // receiver reserve a full-size message during the handshake.
    void set_message_limit(std::size_t bytes);
    void reset();

  private:
    [[nodiscard]] ReceiveResult push_validated(ByteView bytes, const FrameView &frame);
    [[nodiscard]] ReceiveResult fail(ReceiveError error,
                                     std::optional<FrameError> frame_error = std::nullopt);
    FrameLimits limits_;
    MemoryBudget budget_;
    Reassembler reassembler_;
    std::optional<MemoryReservation> reservation_;
    std::optional<MemoryReservation> extra_;
    std::size_t message_limit_;
};

} // namespace p2pdc::detail
