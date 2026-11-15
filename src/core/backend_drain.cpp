#include "backend_drain.hpp"

#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
struct BackendDrain::State : std::enable_shared_from_this<State> {
    explicit State(Executor dispatch) : dispatch(std::move(dispatch)) {}
    Executor dispatch;
    std::mutex mutex;
    std::size_t pending = 0;
    bool draining = false;
    bool sealed = false;
    std::function<void()> drained;
    void submit(std::function<void()> task);
    void complete();
};

void BackendDrain::State::complete() {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex);
        --pending;
        if (draining && pending == 0) {
            sealed = true;
            callback = std::move(drained);
        }
    }
    if (callback)
        callback();
}

void BackendDrain::State::submit(std::function<void()> task) {
    if (!task)
        throw std::invalid_argument("backend task is required");
    std::function<void()> wrapped = [self = shared_from_this(), task = std::move(task)]() mutable {
        try {
            task();
        } catch (...) {
            auto error = std::current_exception();
            self->complete();
            std::rethrow_exception(error);
        }
        self->complete();
    };
    {
        std::lock_guard lock(mutex);
        if (sealed)
            throw std::logic_error("backend executor has drained");
        ++pending;
    }
    try {
        dispatch(std::move(wrapped));
    } catch (...) {
        complete();
        throw;
    }
}

BackendDrain::BackendDrain(Executor executor)
    : state_(std::make_shared<State>(std::move(executor))) {
    if (!state_->dispatch)
        throw std::invalid_argument("backend executor is required");
}
Executor BackendDrain::executor() const {
    return [state = state_](std::function<void()> task) { state->submit(std::move(task)); };
}
bool BackendDrain::begin(std::function<void()> callback) {
    if (!callback)
        throw std::invalid_argument("backend drain callback is required");
    {
        std::lock_guard lock(state_->mutex);
        if (state_->draining)
            return false;
        state_->draining = true;
        if (state_->pending != 0) {
            state_->drained = std::move(callback);
            return true;
        }
        state_->sealed = true;
    }
    callback();
    return true;
}
} // namespace p2pdc::detail
