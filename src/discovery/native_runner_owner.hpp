#pragma once

#include "../core/submission_gate.hpp"
#include "runner_quota.hpp"

#include <opendht.h>

#include <exception>
#include <memory>
#include <variant>

namespace p2pdc::detail {

struct RunnerCanceled {};
using RunnerStartResult =
    std::variant<std::shared_ptr<dht::DhtRunner>, std::exception_ptr, RunnerCanceled>;
using RunnerStart = std::function<void(dht::DhtRunner &)>;

// The start function and join run on a dedicated worker. The returned runner
// remains pinned by that worker until join completes. Close all users of the
// runner before requesting shutdown. Callbacks are posted through the supplied
// executor in submission order; it must be FIFO as well as serialized, and it
// and its captures must outlive admitted submissions. Methods are
// called on the owner executor, not from a DHT callback. The start function must
// return eventually. Destruction suppresses pending callbacks and requests stop;
// it does not join. An already admitted callback may still run. The owner must
// poll delivery_failed() for exceptional executor submission/allocation failure.
// Join is retried indefinitely after an exception. A quota slot remains held
// until the worker exits. The default quota is shared process-wide; callers
// supplying a custom quota must share it across their runtime. There is no
// global drain: await shutdown before tearing down process-wide resources.
// The stopped callback can run just before the worker releases its quota slot;
// immediate replacement may therefore encounter temporary quota exhaustion.
class NativeRunnerOwner {
  public:
    using Started = std::function<void(RunnerStartResult)>;
    using Stopped = std::function<void(std::exception_ptr)>;

    // Returns null if no worker slot is available; other construction failures throw.
    static std::unique_ptr<NativeRunnerOwner> create(Executor executor, RunnerStart start,
                                                     Started started,
                                                     RunnerQuota quota = default_runner_quota());
    ~NativeRunnerOwner();
    NativeRunnerOwner(const NativeRunnerOwner &) = delete;
    NativeRunnerOwner &operator=(const NativeRunnerOwner &) = delete;
    [[nodiscard]] bool shutdown(Stopped stopped);
    [[nodiscard]] bool delivery_failed() const noexcept;

  private:
    struct State;
    explicit NativeRunnerOwner(std::shared_ptr<State> state);
    static void work(std::shared_ptr<State> state, RunnerStart start, Started started);
    static void publish_start(const std::shared_ptr<State> &state, Started started,
                              RunnerStartResult result);
    static void publish_stop(const std::shared_ptr<State> &state, std::exception_ptr error);
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
