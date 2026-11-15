#pragma once

#include "p2pdc/frame.h"
#include "p2pdc/memory_budget.h"

#include <deque>
#include <optional>

namespace p2pdc::detail {

enum class SenderEnqueue { Accepted, TooLarge, Backpressure, AllocationFailed, Closed };
enum class SenderFrameError { Backpressure, AllocationFailed, InvalidFrame };

struct SenderFrame {
    std::optional<ByteView> frame;
    std::optional<SenderFrameError> error;
};

// Serialized caller context. A peeked frame remains valid until commit or clear.
// The caller commits only after the transport has accepted the whole frame.
// clear is terminal; reusing a stream after partial-message discard is unsafe.
// Reserves one frame of scratch space while messages are queued, so an admitted
// message normally makes progress even when the shared budget is otherwise full.
// Reservations cover owned payload and frame vector capacity, not backend
// copies or allocator overhead; excess capacity is charged after allocation and
// may still cause backpressure if the allocator provides more than requested.
class ReliableSender {
  public:
    ReliableSender(FrameLimits limits, MemoryBudget budget, std::size_t max_queued_bytes,
                   std::size_t max_messages);
    [[nodiscard]] SenderEnqueue enqueue(ByteView message);
    [[nodiscard]] bool can_enqueue(std::size_t size) const;
    [[nodiscard]] SenderFrame peek();
    void commit();
    void clear();
    [[nodiscard]] std::size_t queued_bytes() const;
    [[nodiscard]] std::size_t queued_messages() const;

  private:
    [[nodiscard]] SenderEnqueue admit(ByteView message, MemoryReservation reservation,
                                      std::optional<MemoryReservation> scratch);
    struct Message {
        Bytes bytes;
        MemoryReservation reservation;
        std::optional<MemoryReservation> extra;
    };
    FrameLimits limits_;
    MemoryBudget budget_;
    std::size_t max_queued_bytes_;
    std::size_t max_messages_;
    std::size_t queued_bytes_ = 0;
    std::deque<Message> messages_;
    std::size_t offset_ = 0;
    std::uint64_t message_id_ = 0;
    std::optional<Bytes> frame_;
    std::optional<MemoryReservation> frame_reservation_;
    std::optional<MemoryReservation> frame_extra_;
    bool closed_ = false;
};

} // namespace p2pdc::detail
