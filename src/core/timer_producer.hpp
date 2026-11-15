#pragma once

#include "p2pdc/operation.h"
#include "p2pdc/scheduler.h"

#include <memory>

namespace p2pdc::detail {

// One outstanding timer. Methods and delivered callbacks use the same serial
// owner executor. The scheduler may fire from any thread, even during arm().
// cancel/close suppress queued delivery; close also blocks new submissions.
// Submission failure is recorded in healthy(); the owner must poll or close.
// Scheduler calls and cancellation must not synchronously reenter this object.
// The scheduler's nonblocking, nonthrowing cancel contract is required.
class TimerProducer {
  public:
    TimerProducer(Executor executor, Scheduler scheduler);
    ~TimerProducer();
    TimerProducer(const TimerProducer &) = delete;
    TimerProducer &operator=(const TimerProducer &) = delete;
    void arm(std::chrono::milliseconds delay, std::function<void()> callback);
    void cancel();
    void close();
    [[nodiscard]] bool healthy() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
