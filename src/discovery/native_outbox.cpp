#include "native_outbox.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

struct NativeOutbox::State : std::enable_shared_from_this<State> {
    NativePublisher publisher;
    SignalingQueue queue;
    OutboxCallbacks callbacks;
    bool active = true;
    bool inflight = false;
    bool paused = false;
    bool notifying = false;
    bool defer_copy_budget;
    bool waiting_budget = false;

    State(RecordStore store, std::array<std::byte, 20> key, MemoryBudget budget,
          OutboxLimits limits, Executor executor, OutboxCallbacks handlers, bool defer_budget)
        : publisher(std::move(store), key, budget, std::move(executor)),
          queue(std::move(budget), limits.queued_bytes, limits.queued_records, limits.record_bytes),
          callbacks(std::move(handlers)), defer_copy_budget(defer_budget) {}
    void pump();
    void complete(bool success);
    void fail(OutboxError error);
};

void NativeOutbox::State::fail(OutboxError error) {
    inflight = false;
    paused = true;
    notifying = true;
    callbacks.on_error(error);
    notifying = false;
}

void NativeOutbox::State::complete(bool success) {
    if (!active) {
        return;
    }
    inflight = false;
    if (!success) {
        fail(OutboxFailure::BackendFailed);
        return;
    }
    if (!queue.pop()) {
        throw std::logic_error("native outbox lost its in-flight record");
    }
    callbacks.on_published();
    pump();
}

void NativeOutbox::State::pump() {
    if (!active || paused || inflight || !queue.front()) {
        return;
    }
    inflight = true;
    try {
        const auto result = publisher.send(*queue.front(), [weak = weak_from_this()](bool success) {
            if (auto self = weak.lock()) {
                self->complete(success);
            }
        });
        if (auto error = std::get_if<PublishError>(&result)) {
            if (*error == PublishError::ResourceLimit && defer_copy_budget) {
                inflight = false;
                paused = true;
                waiting_budget = true;
            } else {
                fail(*error);
            }
        } else if (auto error = std::get_if<StoreError>(&result)) {
            fail(*error);
        }
    } catch (const std::exception &) {
        fail(OutboxFailure::SubmissionException);
    }
}

NativeOutbox::NativeOutbox(RecordStore store, std::array<std::byte, 20> key, MemoryBudget budget,
                           OutboxLimits limits, Executor executor, OutboxCallbacks callbacks,
                           bool defer_copy_budget)
    : state_(std::make_shared<State>(std::move(store), key, budget, limits, std::move(executor),
                                     std::move(callbacks), defer_copy_budget)) {
    if (!state_->callbacks.on_published || !state_->callbacks.on_error) {
        throw std::invalid_argument("native outbox requires callbacks");
    }
    if (limits.queued_bytes > budget.limit() ||
        limits.record_bytes > budget.limit() - limits.queued_bytes) {
        throw std::invalid_argument("native outbox budget needs room for a publication copy");
    }
}

NativeOutbox::~NativeOutbox() { close(); }

SignalingQueueResult NativeOutbox::enqueue(Bytes record) {
    auto state = state_;
    if (!state->active || !state->publisher.is_open()) {
        return SignalingQueueResult::Closed;
    }
    auto result = state->queue.push(std::move(record));
    if (result == SignalingQueueResult::Accepted) {
        state->pump();
    }
    return result;
}

bool NativeOutbox::retry() {
    auto state = state_;
    if (!state->active || !state->publisher.is_open() || !state->paused || state->notifying) {
        return false;
    }
    state->paused = false;
    state->waiting_budget = false;
    state->pump();
    return true;
}

bool NativeOutbox::retry_budget() { return state_->waiting_budget && retry(); }

bool NativeOutbox::is_open() const { return state_->active && state_->publisher.is_open(); }
std::size_t NativeOutbox::size() const { return state_->queue.size(); }
std::size_t NativeOutbox::buffered_bytes() const { return state_->queue.buffered_bytes(); }
void NativeOutbox::close() {
    state_->active = false;
    state_->publisher.close();
    state_->queue.close();
}

} // namespace p2pdc::detail
