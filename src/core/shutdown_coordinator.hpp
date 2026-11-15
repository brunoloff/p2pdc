#pragma once

#include "p2pdc/callback_gate.h"

#include <cstdint>
#include <map>
#include <optional>

namespace p2pdc::detail {

// Owner-executor only. The executor must be asynchronous, serial, FIFO,
// nonblocking and nonthrowing. Register each producer before starting it; its
// idempotent stop action must prevent any later submissions to the executor.
// Register each operation until its completion has been admitted to gate().
// All user callbacks, including operation completions, must use gate(). Queued
// backend work must not submit to the raw executor after producers are stopped.
// Do not close gate() directly; admission closes when the shutdown fence runs.
// Stop actions must not destroy the coordinator while its sweep is running.
// This coordinates graceful shutdown; it does not implement handle abandonment.
class ShutdownCoordinator {
  public:
    explicit ShutdownCoordinator(Executor executor);
    [[nodiscard]] std::optional<std::uint64_t> track(std::function<void()> stop);
    void retire(std::uint64_t id);
    [[nodiscard]] CallbackGate gate() const;
    [[nodiscard]] bool begin(std::function<void()> final_callback);
    [[nodiscard]] bool retry();

  private:
    CallbackGate gate_;
    std::map<std::uint64_t, std::shared_ptr<std::function<void()>>> tracked_;
    std::function<void()> final_;
    std::uint64_t next_id_ = 1;
    bool closing_ = false;
    bool closed_ = false;
    bool sweeping_ = false;
};
} // namespace p2pdc::detail
