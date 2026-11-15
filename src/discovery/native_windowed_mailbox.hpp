#pragma once

#include "discovery_window.hpp"
#include "native_mailbox.hpp"
#include "recovering_mailbox.hpp"

namespace p2pdc::detail {
enum class WindowedMailboxError {
    ClockRegressed,
    InvalidRange,
    ResourceLimit,
    Crypto,
    Store,
    Registration,
    Catchup
};
struct WindowedMailboxCallbacks {
    std::function<void(Bytes)> on_record;
    std::function<void(WindowedMailboxError, std::optional<StoreError>)> on_error;
    std::function<void(bool)> on_catchup;
    std::function<void(RecoveryState)> on_status{};
};

// Experimental coordinator. All methods and callbacks use one serialized
// executor; callbacks must not reenter or destroy the coordinator. tick uses
// nondecreasing UTC seconds, opens each acceptable issue-time window, and
// starts catch-up after its registration is ready. on_catchup reports once per
// current window range after all its lookups finish; false means at least one
// lookup failed. Records can arrive before or after that callback. Old windows
// are closed after new watches are submitted; DHT delivery is best effort.
// The shared budget bounds queued payloads across all windows. Backend memory
// and up to twice max_discovery_windows transient watches are not covered.
class NativeWindowedMailbox {
  public:
    NativeWindowedMailbox(RecordStore store, std::shared_ptr<const Bytes> capability,
                          MailboxLimits limits, std::uint64_t max_lifetime,
                          std::uint64_t clock_skew, MemoryBudget budget, Executor executor,
                          WindowedMailboxCallbacks callbacks,
                          std::optional<RecoveryOptions> recovery = {});
    ~NativeWindowedMailbox();
    NativeWindowedMailbox(const NativeWindowedMailbox &) = delete;
    NativeWindowedMailbox &operator=(const NativeWindowedMailbox &) = delete;
    [[nodiscard]] bool tick(std::uint64_t now);
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool recovering() const;
    void close();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
