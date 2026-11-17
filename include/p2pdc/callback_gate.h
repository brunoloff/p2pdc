#pragma once

#include "p2pdc/operation.h"

#include <functional>
#include <memory>

namespace p2pdc {

namespace detail {
struct CallbackGateState;
}

// A gate shares the executor's asynchronous, serial submission contract. If the
// executor throws, post() rethrows and the callback is not admitted. close()
// rejects new posts and invokes its final callback only after all admitted
// callbacks return, including submissions racing with close(). If the executor
// rejects a deferred final callback, the failure is logged and it cannot run.
// It does not itself cancel backend operations or prevent separate ungated posts.
// abandon() suppresses callbacks not yet admitted for execution, including the
// final callback. A callback concurrently admitted before abandonment may run.
// Abandonment does not wait for callbacks or release queued captures immediately.
// A submission already in flight may reach the executor after abandonment; use
// a separate producer barrier when executor submission itself must have stopped.
// Stored callbacks must not strongly capture their own gate or its owner; doing
// so can create a cycle that abandonment alone cannot break.
class CallbackGate {
  public:
    explicit CallbackGate(Executor executor);
    [[nodiscard]] bool post(std::function<void()> callback) const;
    [[nodiscard]] bool close(std::function<void()> final_callback) const;
    void abandon() const noexcept;

  private:
    std::shared_ptr<detail::CallbackGateState> state_;
};

} // namespace p2pdc
