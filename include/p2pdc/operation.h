#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>

namespace p2pdc {

enum class OperationError { Canceled };
using Unit = std::monostate; // CompletionSource<Unit> represents a void result.
template <class T> using OperationResult = std::variant<T, OperationError>;
using Executor = std::function<void(std::function<void()>)>;

namespace detail {
template <class T> struct OperationState {
    std::mutex mutex;
    bool finished = false;
    std::shared_ptr<std::atomic_bool> abandoned = std::make_shared<std::atomic_bool>(false);
    Executor executor;
    std::function<void(OperationResult<T>)> callback;

    bool finish(OperationResult<T> result) {
        {
            std::lock_guard lock(mutex);
            if (finished || abandoned->load()) {
                return false;
            }
        }
        // User result moves and destructors may reenter this operation.
        auto payload = std::make_shared<OperationResult<T>>(std::move(result));
        auto deliver = std::make_shared<std::function<void(OperationResult<T>)>>();
        std::function<void()> task = [payload, deliver, abandoned = abandoned]() mutable {
            if (!abandoned->load()) {
                (*deliver)(std::move(*payload));
            }
        };
        std::unique_lock lock(mutex);
        if (finished || abandoned->load()) {
            return false;
        }
        *deliver = std::move(callback);
        finished = true;
        try {
            executor(std::move(task));
        } catch (...) {
            callback = std::move(*deliver);
            finished = false;
            throw;
        }
        return true;
    }
};
} // namespace detail

template <class T> class CompletionSource;

// Executor must accept submissions safely from backend threads without blocking
// or throwing, post asynchronously (never inline), and serialize callbacks.
// Submission must not synchronously reenter this operation's cancel/complete.
// If submission throws without accepting the task, completion is restored and
// can be retried with a new result or canceled. The failed result is consumed.
// An executor must never accept a task and then throw.
// Shutdown must prevent new operation registration, cancel every tracked operation
// while its callback gate is open, and only then close that gate.
template <class T> class Operation {
  public:
    [[nodiscard]] bool cancel() const {
        auto state = state_;
        return state->finish(OperationError::Canceled);
    }

  private:
    friend class CompletionSource<T>;
    explicit Operation(std::shared_ptr<detail::OperationState<T>> state)
        : state_(std::move(state)) {}
    std::shared_ptr<detail::OperationState<T>> state_;
};

template <class T> class CompletionSource {
  public:
    CompletionSource(Executor executor, std::function<void(OperationResult<T>)> callback)
        : state_(std::make_shared<detail::OperationState<T>>()) {
        if (!executor || !callback) {
            throw std::invalid_argument("operation requires an executor and completion callback");
        }
        state_->executor = std::move(executor);
        state_->callback = std::move(callback);
    }
    [[nodiscard]] Operation<T> operation() const { return Operation<T>(state_); }
    [[nodiscard]] bool complete(OperationResult<T> result) const {
        auto state = state_;
        return state->finish(std::move(result));
    }
    // Suppress delivery without waiting; a callback already admitted may run.
    // Queued tasks may retain their captures until the executor releases them.
    // An unfinished operation retains its callback while its state is alive.
    // A pending callback must not strongly capture its source or owner: this
    // can form a cycle, and abandonment does not discard the stored callback.
    void abandon() const noexcept { state_->abandoned->store(true); }

  private:
    std::shared_ptr<detail::OperationState<T>> state_;
};

} // namespace p2pdc
