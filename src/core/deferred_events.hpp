#pragma once

#include "p2pdc/memory_budget.h"
#include "p2pdc/operation.h"

#include <memory>

namespace p2pdc::detail {

enum class DeferredEventResult { Accepted, Full, Closed };

// Methods and callbacks run on one serialized executor; submission is
// asynchronous, nonblocking and nonthrowing. Callbacks may close or destroy
// the queue. Closing suppresses events not yet delivered. Caller supplies the
// size of each retained payload; function captures and allocator overhead are
// not accounted. Exceptions while storing or submitting an event propagate and
// close the queue. Do not double-charge buffers already reserved elsewhere;
// transferring an existing reservation requires a separate ownership path.
// A Full result must be handled explicitly by the owner. This queue discards
// pending events on close and is not suitable for mandatory cancellation or
// shutdown completions without a separate delivery guarantee.
// A queue shared by several handles also needs per-handle delivery guards;
// closing one handle cannot selectively remove its entries from this queue.
class DeferredEvents {
  public:
    DeferredEvents(Executor executor, MemoryBudget budget, std::size_t max_events,
                   std::size_t max_bytes);
    ~DeferredEvents();
    DeferredEvents(const DeferredEvents &) = delete;
    DeferredEvents &operator=(const DeferredEvents &) = delete;
    [[nodiscard]] DeferredEventResult post(std::size_t payload_bytes, std::function<void()> event);
    void close();
    [[nodiscard]] std::size_t pending() const;
    // Whether an event of payload_bytes would be accepted with a slot to spare for another,
    // or the queue is empty and would take it.
    [[nodiscard]] bool has_room(std::size_t payload_bytes) const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
