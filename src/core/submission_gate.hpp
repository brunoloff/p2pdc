#pragma once

#include "p2pdc/operation.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace p2pdc::detail {

// Executor submission must be nonblocking, nonthrowing, asynchronous and must
// not reenter this gate. close waits for a submission already in progress; it
// does not wait for tasks that were already accepted by the executor.
// An executor that runs a task inline, on the posting thread before returning,
// would re-enter the gate and its owner and deadlock; post detects that, does
// not run the task, and throws std::logic_error instead.
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
        auto poster = std::make_shared<std::atomic<std::thread::id>>(std::this_thread::get_id());
        executor_([poster, task = std::move(task)] {
            if (poster->load() == std::this_thread::get_id()) {
                throw std::logic_error("executor ran a task inline; p2pdc executors must post "
                                       "tasks asynchronously");
            }
            task();
        });
        poster->store(std::thread::id{});
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
