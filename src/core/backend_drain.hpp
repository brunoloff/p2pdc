#pragma once

#include "p2pdc/operation.h"

#include <memory>

namespace p2pdc::detail {
// Tracks backend tasks, including work posted by already-admitted tasks. The
// owner stops external producers before begin(); the executor is serialized,
// asynchronous and nonthrowing. begin() and the drained callback use the owner
// executor. Once drained, retained executor handles reject later submissions.
class BackendDrain {
  public:
    explicit BackendDrain(Executor executor);
    [[nodiscard]] Executor executor() const;
    [[nodiscard]] bool begin(std::function<void()> drained);

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
