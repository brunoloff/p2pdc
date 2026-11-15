#pragma once

#include "native_mailbox.hpp"

#include <chrono>

namespace p2pdc::detail {
enum class RecoveryState { Opening, CatchingUp, Ready, Reconnecting, Failed, Closed };
enum class RecoveryError { Backend, ResourceLimit, Clock };
struct RecoveryOptions {
    std::size_t max_retries = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{60000};
};
struct RecoveryCallbacks {
    std::function<void(Bytes)> on_record;
    std::function<void()> on_catchup;
    std::function<void(RecoveryState)> on_status;
    std::function<void(RecoveryError)> on_error;
};

// Serialized, non-reentrant owner. poll() is driven by the owner's timer and
// uses a nondecreasing monotonic time. Callbacks must not throw, reenter or
// destroy this object. Reopening always registers before a fresh catch-up;
// neither operation proves that the DHT returned every record. Records can
// arrive before catch-up. close suppresses callbacks and cancels the watch.
class RecoveringMailbox {
  public:
    using Clock = std::chrono::steady_clock;
    RecoveringMailbox(RecordStore store, std::array<std::byte, 20> key, MailboxLimits limits,
                      MemoryBudget budget, Executor executor, RecoveryOptions options,
                      RecoveryCallbacks callbacks,
                      LookupDeduplication deduplication = LookupDeduplication::Content);
    ~RecoveringMailbox();
    RecoveringMailbox(const RecoveringMailbox &) = delete;
    RecoveringMailbox &operator=(const RecoveringMailbox &) = delete;
    [[nodiscard]] bool poll(Clock::time_point now);
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] RecoveryState state() const;
    void close();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
