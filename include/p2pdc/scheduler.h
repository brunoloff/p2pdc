#pragma once

#include <chrono>
#include <functional>

namespace p2pdc {

using CancelTimer = std::function<void()>;
// Schedules a one-shot callback after a monotonic delay. The returned cancel
// function is nonblocking, nonthrowing and idempotent; cancellation may race
// with a callback already in flight. Scheduling must not block on callback
// execution; scheduling and cancellation must not synchronously reenter the
// timer's owner. Scheduling must either return a nonempty cancel function or throw without
// retaining a callback. Callbacks may run on a backend thread and may race with
// the return of the scheduler. Implementations must not log secrets captured by
// callbacks. A canceled callback may remain retained until the scheduler drops
// it. The scheduler and executor outlive pending backend callbacks.
using Scheduler = std::function<CancelTimer(std::chrono::milliseconds, std::function<void()>)>;

} // namespace p2pdc
