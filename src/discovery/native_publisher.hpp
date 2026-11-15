#pragma once

#include "record_store.hpp"
#include "p2pdc/memory_budget.h"
#include "p2pdc/operation.h"

#include <array>

namespace p2pdc::detail {

enum class PublishError { Closed, Busy, ResourceLimit };
using PublishResult = std::variant<std::monostate, PublishError, StoreError>;

// One outstanding write. Methods and nonthrowing completions run on a single
// serialized context; executor submission is nonblocking, nonthrowing,
// asynchronous, and does not synchronously reenter the publisher. Backend
// callbacks may run on another thread or inline. Budget accounts for submitted
// payload bytes until completion runs or a late completion is discarded; it excludes OpenDHT
// allocations, retained values, replicas, and callback overhead. Retain the exact encoded record
// externally for retries. Completion indicates publication only, not peer receipt. Close cannot
// cancel or retract a put. Once close returns, no backend completion can submit another task. Tasks
// already submitted may still run and must be drained by the owner.
// If posting fails exceptionally, is_open becomes false and an error is logged.
class NativePublisher {
  public:
    NativePublisher(RecordStore store, std::array<std::byte, 20> key, MemoryBudget budget,
                    Executor executor);
    ~NativePublisher();
    NativePublisher(const NativePublisher &) = delete;
    NativePublisher &operator=(const NativePublisher &) = delete;
    [[nodiscard]] PublishResult send(ByteView record, std::function<void(bool)> completion);
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
