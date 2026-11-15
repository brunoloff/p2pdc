#pragma once

#include "p2pdc/frame.h"
#include "p2pdc/memory_budget.h"

#include <deque>
#include <optional>

namespace p2pdc::detail {

enum class SignalingQueueResult { Accepted, TooLarge, Full, AllocationFailed, Closed };

// Serialized, bounded queue of already encoded records. The caller removes a
// record only after publication succeeds; a failed write leaves front intact
// for retry. Admission consumes the argument even on failure, so the caller
// must retain a copy or fail the attempt if admission fails. Reservations cover
// vector capacity until removal, excluding container and backend overhead.
class SignalingQueue {
  public:
    SignalingQueue(MemoryBudget budget, std::size_t max_bytes, std::size_t max_records,
                   std::size_t max_record_bytes);
    [[nodiscard]] SignalingQueueResult push(Bytes record);
    [[nodiscard]] std::optional<ByteView> front() const;
    [[nodiscard]] bool pop();
    void close();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t buffered_bytes() const;

  private:
    struct Record {
        Bytes data;
        MemoryReservation reservation;
    };
    MemoryBudget budget_;
    std::size_t max_bytes_;
    std::size_t max_records_;
    std::size_t max_record_bytes_;
    std::size_t bytes_ = 0;
    std::deque<Record> records_;
    bool closed_ = false;
};

} // namespace p2pdc::detail
