#pragma once

#include "native_publisher.hpp"
#include "signaling_queue.hpp"

namespace p2pdc::detail {

enum class OutboxFailure { BackendFailed, SubmissionException };
using OutboxError = std::variant<OutboxFailure, PublishError, StoreError>;

struct OutboxLimits {
    std::size_t queued_bytes;
    std::size_t queued_records;
    std::size_t record_bytes;
};

struct OutboxCallbacks {
    std::function<void()> on_published;
    std::function<void(OutboxError)> on_error;
};

// Methods and nonthrowing callbacks run on the publisher's serialized executor.
// Failed writes pause with their original bytes retained. retry is explicit;
// callers choose delay and a deadline. enqueue consumes its record on failure.
// Queue reservations and the active publisher reservation share the budget;
// configuration must leave room for an additional copy of the largest record;
// other users of the same budget can still cause temporary admission failure.
// OpenDHT may retain values after close. Publication does not imply receipt.
// A publisher executor-submission failure is visible via is_open; callers need
// a deadline because no callback can be guaranteed in that exceptional case.
// When defer_copy_budget is enabled, a copy-budget failure becomes a
// recoverable wait; the owner must call retry_budget and enforce a deadline.
class NativeOutbox {
  public:
    NativeOutbox(RecordStore store, std::array<std::byte, 20> key, MemoryBudget budget,
                 OutboxLimits limits, Executor executor, OutboxCallbacks callbacks,
                 bool defer_copy_budget = false);
    ~NativeOutbox();
    NativeOutbox(const NativeOutbox &) = delete;
    NativeOutbox &operator=(const NativeOutbox &) = delete;
    [[nodiscard]] SignalingQueueResult enqueue(Bytes record);
    [[nodiscard]] bool retry();
    [[nodiscard]] bool retry_budget();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t buffered_bytes() const;
    void close();

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
