#include "native_publisher.hpp"
#include "../core/submission_gate.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

struct NativePublisher::State {
    RecordStore store;
    std::array<std::byte, 20> key;
    MemoryBudget budget;
    SubmissionGate submissions;
    std::atomic_bool active{true};
    bool busy = false;

    State(RecordStore source, std::array<std::byte, 20> address, MemoryBudget capacity,
          Executor dispatch)
        : store(std::move(source)), key(address), budget(std::move(capacity)),
          submissions(std::move(dispatch)) {}

    void complete(const std::shared_ptr<MemoryReservation> &reservation,
                  const std::function<void(bool)> &callback, bool success);
    static void post(std::shared_ptr<State> state, std::shared_ptr<MemoryReservation> reservation,
                     const std::function<void(bool)> &callback, bool success);
};

void NativePublisher::State::complete(const std::shared_ptr<MemoryReservation> &reservation,
                                      const std::function<void(bool)> &callback, bool success) {
    busy = false;
    reservation->release();
    if (active) {
        callback(success);
    }
}

void NativePublisher::State::post(std::shared_ptr<State> state,
                                  std::shared_ptr<MemoryReservation> reservation,
                                  const std::function<void(bool)> &callback, bool success) {
    try {
        if (!state->submissions.post([state, reservation, callback, success] {
                state->complete(reservation, callback, success);
            })) {
            reservation->release();
        }
    } catch (const std::exception &error) {
        state->active = false;
        reservation->release();
        std::cerr << "NativePublisher: cannot submit completion: " << error.what() << '\n';
    }
}

NativePublisher::NativePublisher(RecordStore store, std::array<std::byte, 20> key,
                                 MemoryBudget budget, Executor executor)
    : state_(
          std::make_shared<State>(std::move(store), key, std::move(budget), std::move(executor))) {}

NativePublisher::~NativePublisher() { close(); }

PublishResult NativePublisher::send(ByteView record, std::function<void(bool)> completion) {
    if (!completion) {
        throw std::invalid_argument("native publisher requires a completion callback");
    }
    auto state = state_;
    if (!state->active) {
        return PublishError::Closed;
    }
    if (state->busy) {
        return PublishError::Busy;
    }
    auto reserved = state->budget.try_reserve(record.size());
    if (!reserved) {
        return PublishError::ResourceLimit;
    }
    auto reservation = std::make_shared<MemoryReservation>(std::move(*reserved));
    state->busy = true;
    try {
        auto error = state->store.put(
            state->key, record,
            [state, reservation, completion = std::move(completion)](bool success) {
                State::post(state, reservation, completion, success);
            });
        if (error) {
            state->busy = false;
            return *error;
        }
    } catch (...) {
        state->busy = false;
        throw;
    }
    return std::monostate{};
}

bool NativePublisher::is_open() const noexcept { return state_->active.load(); }

void NativePublisher::close() noexcept {
    state_->active = false;
    state_->submissions.close();
}

} // namespace p2pdc::detail
