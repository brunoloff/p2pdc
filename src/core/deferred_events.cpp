#include "deferred_events.hpp"

#include <deque>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
struct DeferredEvents::State : std::enable_shared_from_this<State> {
    struct Event {
        std::function<void()> callback;
        MemoryReservation reservation;
    };
    Executor executor;
    MemoryBudget budget;
    std::size_t max_events;
    std::size_t max_bytes;
    std::size_t count = 0;
    std::size_t bytes = 0;
    std::deque<Event> events;
    bool active = true;
    bool scheduled = false;

    State(Executor dispatch, MemoryBudget capacity, std::size_t count_limit, std::size_t byte_limit)
        : executor(std::move(dispatch)), budget(std::move(capacity)), max_events(count_limit),
          max_bytes(byte_limit) {}
    void close();
    void schedule();
    void deliver();
};

void DeferredEvents::State::close() {
    if (!active)
        return;
    active = false;
    while (!events.empty()) {
        auto event = std::move(events.front());
        events.pop_front();
        --count;
        bytes -= event.reservation.bytes();
    }
}

void DeferredEvents::State::schedule() {
    scheduled = true;
    try {
        executor([weak = weak_from_this()] {
            if (auto self = weak.lock())
                self->deliver();
        });
    } catch (...) {
        scheduled = false;
        close();
        throw;
    }
}

void DeferredEvents::State::deliver() {
    if (!active || events.empty()) {
        scheduled = false;
        return;
    }
    auto event = std::move(events.front());
    events.pop_front();
    try {
        event.callback();
    } catch (...) {
        --count;
        bytes -= event.reservation.bytes();
        scheduled = false;
        close();
        throw;
    }
    --count;
    bytes -= event.reservation.bytes();
    if (active && !events.empty())
        schedule();
    else
        scheduled = false;
}

DeferredEvents::DeferredEvents(Executor executor, MemoryBudget budget, std::size_t max_events,
                               std::size_t max_bytes)
    : state_(
          std::make_shared<State>(std::move(executor), std::move(budget), max_events, max_bytes)) {
    if (!state_->executor || !max_events)
        throw std::invalid_argument("deferred events require an executor and nonzero count limit");
}
DeferredEvents::~DeferredEvents() { close(); }

DeferredEventResult DeferredEvents::post(std::size_t payload_bytes, std::function<void()> event) {
    if (!event)
        throw std::invalid_argument("deferred event callback is required");
    auto state = state_;
    if (!state->active)
        return DeferredEventResult::Closed;
    if (state->count >= state->max_events || payload_bytes > state->max_bytes - state->bytes)
        return DeferredEventResult::Full;
    auto reservation = state->budget.try_reserve(payload_bytes);
    if (!reservation)
        return DeferredEventResult::Full;
    try {
        state->events.push_back({std::move(event), std::move(*reservation)});
    } catch (...) {
        state->close();
        throw;
    }
    ++state->count;
    state->bytes += payload_bytes;
    if (!state->scheduled)
        state->schedule();
    return DeferredEventResult::Accepted;
}

void DeferredEvents::close() {
    auto state = state_;
    state->close();
}
std::size_t DeferredEvents::pending() const { return state_->count; }
} // namespace p2pdc::detail
