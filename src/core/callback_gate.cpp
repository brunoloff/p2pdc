#include "p2pdc/callback_gate.h"

#include <atomic>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
struct CallbackGateState {
    std::mutex mutex;
    Executor executor;
    std::function<void()> final_callback;
    std::shared_ptr<std::atomic_bool> abandoned = std::make_shared<std::atomic_bool>(false);
    std::size_t pending = 0;
    bool closing = false;
    bool final_scheduled = false;
};

void finish_callback(const std::shared_ptr<CallbackGateState> &state) {
    std::function<void()> final;
    {
        std::lock_guard lock(state->mutex);
        --state->pending;
        if (state->closing && state->pending == 0 && !state->final_scheduled) {
            state->final_scheduled = true;
            final = std::move(state->final_callback);
        }
    }
    if (final && !state->abandoned->load()) {
        state->executor(std::move(final));
    }
}

struct FinishOnExit {
    std::shared_ptr<CallbackGateState> state;
    ~FinishOnExit() { finish_callback(state); }
};
} // namespace p2pdc::detail

namespace p2pdc {
CallbackGate::CallbackGate(Executor executor)
    : state_(std::make_shared<detail::CallbackGateState>()) {
    if (!executor) {
        throw std::invalid_argument("callback gate requires an executor");
    }
    state_->executor = std::move(executor);
}

bool CallbackGate::post(std::function<void()> callback) const {
    if (!callback) {
        throw std::invalid_argument("callback gate requires a callback");
    }
    auto state = state_;
    std::function<void()> task = [state, callback = std::move(callback)] {
        detail::FinishOnExit finish{state};
        if (!state->abandoned->load()) {
            callback();
        }
    };
    {
        std::lock_guard lock(state->mutex);
        if (state->closing || state->abandoned->load()) {
            return false;
        }
        ++state->pending;
    }
    try {
        state->executor(std::move(task));
    } catch (...) {
        // A rejected submission must release its slot, or close() would never
        // deliver its final callback.
        auto error = std::current_exception();
        try {
            detail::finish_callback(state);
        } catch (const std::exception &final_error) {
            std::cerr << "p2pdc: callback gate could not schedule its final callback: "
                      << final_error.what() << '\n';
        }
        std::rethrow_exception(error);
    }
    return true;
}

bool CallbackGate::close(std::function<void()> final_callback) const {
    if (!final_callback) {
        throw std::invalid_argument("callback gate requires a final callback");
    }
    auto state = state_;
    std::function<void()> guarded_final =
        [abandoned = state->abandoned, callback = std::move(final_callback)] {
            if (!abandoned->load()) {
                callback();
            }
        };
    bool schedule = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->closing || state->abandoned->load()) {
            return false;
        }
        state->closing = true;
        state->final_callback = std::move(guarded_final);
        if (state->pending == 0) {
            state->final_scheduled = true;
            final_callback = std::move(state->final_callback);
            schedule = true;
        }
    }
    if (schedule) {
        state->executor(std::move(final_callback));
    }
    return true;
}

void CallbackGate::abandon() const noexcept { state_->abandoned->store(true); }
} // namespace p2pdc
