#pragma once

#include "p2pdc/operation.h"
#include "p2pdc/scheduler.h"

#include <memory>

namespace p2pdc {

// Serial event loop. Native nodes and callbacks run on its worker; WebAssembly
// uses the browser event loop. Dispatch application work with executor().
// Timer callbacks run on the same executor. Browser callers must use the
// event-loop thread and must not block it.
// Stop and destroy all Nodes before destroying the runtime. Destruction drops
// queued work; native destruction on another thread waits for a running task.
// Destruction on the native worker returns and it exits after its current task.
// Tasks must not throw. Posting after destruction and scheduling after shutdown
// throw; allocation failures are also reported by throwing before acceptance.
// Canceled timer captures are released asynchronously by the worker, subject
// to worker progress; cancellation may race with a timer already executing.
class Runtime {
  public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    [[nodiscard]] Executor executor() const;
    [[nodiscard]] Scheduler scheduler() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace p2pdc
