#pragma once

#include "p2pdc/operation.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {

// Executor submission must be nonblocking, nonthrowing, asynchronous and must
// not reenter this gate. close waits for a submission already in progress; it
// does not wait for tasks that were already accepted by the executor.
class SubmissionGate {
  public:
    explicit SubmissionGate(Executor executor) : executor_(std::move(executor)) {
        if (!executor_) {
            throw std::invalid_argument("submission gate requires an executor");
        }
    }

    bool post(std::function<void()> task) {
        if (!task) {
            throw std::invalid_argument("submission gate requires a task");
        }
        std::lock_guard lock(mutex_);
        if (closed_) {
            return false;
        }
        executor_(std::move(task));
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }

  private:
    std::mutex mutex_;
    Executor executor_;
    bool closed_ = false;
};

} // namespace p2pdc::detail
