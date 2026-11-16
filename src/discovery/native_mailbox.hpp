#pragma once

#include "../transport/inbound_queue.hpp"
#include "p2pdc/operation.h"
#include "record_store.hpp"

#include <array>

namespace p2pdc::detail {

enum class MailboxError { StoreFailure, QueueFull, RecordTooLarge, AllocationFailed };
enum class CatchupError { RegistrationPending, RegistrationFailed, AlreadyStarted, Closed };
using CatchupResult = std::variant<std::monostate, CatchupError, StoreError>;

struct MailboxLimits {
    std::size_t queued_bytes;
    std::size_t queued_records;
    std::size_t record_bytes;
};
struct MailboxCallbacks {
    std::function<void(Bytes)> on_record;
    std::function<void(MailboxError, std::optional<StoreError>)> on_error;
    std::function<void(bool)> on_catchup;
};

// Internal raw mailbox; records are untrusted and may be duplicated, reordered,
// stale, or absent. The consumer must verify and deduplicate them. Records over
// the size, count or budget limits are dropped rather than closing the mailbox,
// so a flood can delay signaling but cannot end it. Methods and
// delivered callbacks use a serialized, asynchronous, FIFO executor whose
// submission is nonblocking, nonthrowing and does not synchronously reenter
// the mailbox;
// backend submissions can come from other threads. User callbacks do not throw.
// Owner keeps the OpenDHT runner running until all work is canceled, and closes
// this mailbox before stopping the runner. Queue budgets exclude backend memory.
// Start the watch, then call catch_up only once registration is ready: this
// starts lookup after local registration without blocking an event loop; DHT
// delivery remains best effort. on_catchup reports lookup completion, not a
// delivery barrier: queued records can arrive after it. An invalid lookup batch
// reports on_error instead of on_catchup, even if earlier records are queued.
// A failed executor
// submission deactivates the mailbox and logs an error; callers must check
// is_open or use a deadline if they need to detect that exceptional failure.
// Once close returns, no backend completion can submit another task. Tasks
// already submitted may still run and must be drained by the owner.
class NativeMailbox {
  public:
    static std::variant<std::unique_ptr<NativeMailbox>, StoreError>
    create(RecordStore store, std::array<std::byte, 20> key, MailboxLimits limits,
           MemoryBudget budget, Executor executor, MailboxCallbacks callbacks,
           LookupDeduplication deduplication = LookupDeduplication::IdAndContent);
    ~NativeMailbox();
    NativeMailbox(const NativeMailbox &) = delete;
    NativeMailbox &operator=(const NativeMailbox &) = delete;
    [[nodiscard]] std::shared_future<std::size_t> registration() const;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::optional<StoreError> pending_store_error() const;
    [[nodiscard]] CatchupResult catch_up();
    void close();

  private:
    struct Impl;
    explicit NativeMailbox(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace p2pdc::detail
