#pragma once

#include "p2pdc/frame.h"
#include "p2pdc/memory_budget.h"

#include <deque>
#include <mutex>
#include <optional>

namespace p2pdc::detail {

enum class InboundStatus { Accepted, Idle, Closed, TooLarge, Full, AllocationFailed, InvalidType };

struct InboundPush {
    InboundStatus status;
    bool schedule_drain;
};

struct InboundMessage {
    Bytes payload;
    MemoryReservation reservation;
};

using InboundNext = std::variant<InboundMessage, InboundStatus>;

// One scheduled drain owns the right to call next until Idle or Closed. After a
// bounded batch it may reschedule itself without relinquishing that right. A
// terminal limit/allocation error discards queued records and is returned once.
// Reservations account for vector capacity, not allocator/backend overhead.
// Optional content deduplication skips only copies still waiting in the queue.
class InboundQueue {
  public:
    InboundQueue(MemoryBudget budget, std::size_t max_bytes, std::size_t max_messages,
                 std::size_t max_message_size);
    InboundQueue(const InboundQueue &) = delete;
    InboundQueue &operator=(const InboundQueue &) = delete;
    [[nodiscard]] InboundPush push(Bytes &&message, bool deduplicate_content = false);
    [[nodiscard]] InboundPush reject_invalid_type();
    [[nodiscard]] InboundNext next();
    void close();
    [[nodiscard]] std::size_t buffered_bytes() const;

  private:
    [[nodiscard]] InboundPush fail(InboundStatus error);
    MemoryBudget budget_;
    std::size_t max_bytes_;
    std::size_t max_messages_;
    std::size_t max_message_size_;
    mutable std::mutex mutex_;
    std::deque<InboundMessage> messages_;
    std::size_t bytes_ = 0;
    std::optional<InboundStatus> error_;
    bool draining_ = false;
    bool closed_ = false;
};

} // namespace p2pdc::detail
