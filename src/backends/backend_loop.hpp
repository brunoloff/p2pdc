#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace p2pdc::detail {

// One worker thread that runs every network backend client of a node, so that each client's
// protocol logic runs on one thread, like its JavaScript original (see
// docs/DISCOVERY_BACKENDS.md). Tasks run in order of their due time, then submission order.
// Tasks must not block; callbacks into p2pdc may run on this thread. The thread starts with the
// first task and ends with stop(), which drops the tasks not yet run.
class BackendLoop {
  public:
    using TimerId = std::uint64_t;
    BackendLoop();
    ~BackendLoop();
    BackendLoop(const BackendLoop &) = delete;
    BackendLoop &operator=(const BackendLoop &) = delete;

    // False once stopped: the task will never run.
    bool post(std::function<void()> task);
    // Zero once stopped. A timer may be canceled until it runs.
    TimerId post_after(std::chrono::milliseconds delay, std::function<void()> task);
    void cancel(TimerId timer);
    [[nodiscard]] bool on_loop_thread() const;
    // Runs no task after returning, except one already running on another thread, which it
    // waits for; from the loop thread itself it returns at once and the thread ends after the
    // current task.
    void stop();

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
