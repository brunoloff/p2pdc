#pragma once

#include "native_mailbox.hpp"
#include "native_outbox.hpp"

namespace p2pdc::detail {

struct SignalPipeLimits {
    MailboxLimits incoming;
    OutboxLimits outgoing;
};
struct SignalPipeCallbacks {
    std::function<void(Bytes)> on_record;
    std::function<void(MailboxError, std::optional<StoreError>)> on_incoming_error;
    std::function<void(OutboxError)> on_outgoing_error;
    std::function<void()> on_published;
    std::function<void(bool)> on_catchup;
};

// Internal transport for untrusted signaling bytes. Methods and callbacks use
// the same serialized executor; callbacks must not throw or reenter the pipe.
// Incoming data may be duplicated, reordered, stale or absent: verify it before
// applying it. Wait for successful registration before catch_up. The owner
// keeps the runner and executor alive until pending backend work is finished.
// Up to eight records may be published concurrently; queue limits apply across
// all publications, and the shared budget covers their copies. Outgoing records
// may reach the peer in any order. If there is no room for a publication copy,
// an admitted record waits behind an existing publication. The owner must call
// retry_budget periodically to resume waits after shared budget is released.
// send may report an outgoing error inline. close suppresses later callbacks but
// cannot retract an OpenDHT publication. Use deadlines and poll is_open for
// exceptional executor failures that cannot reliably deliver a callback.
// An incoming failure does not stop pending outgoing writes; the owner must
// close the pipe when aborting the attempt.
class NativeSignalPipe {
  public:
    static std::variant<std::unique_ptr<NativeSignalPipe>, StoreError>
    create(RecordStore store, std::array<std::byte, 20> local_key,
           std::array<std::byte, 20> remote_key, MemoryBudget budget, SignalPipeLimits limits,
           Executor executor, SignalPipeCallbacks callbacks);
    ~NativeSignalPipe();
    NativeSignalPipe(const NativeSignalPipe &) = delete;
    NativeSignalPipe &operator=(const NativeSignalPipe &) = delete;
    [[nodiscard]] std::shared_future<std::size_t> registration() const;
    [[nodiscard]] CatchupResult catch_up();
    [[nodiscard]] SignalingQueueResult send(Bytes record);
    [[nodiscard]] bool retry();
    void retry_budget();
    [[nodiscard]] bool is_open() const;
    void close();

  private:
    struct State;
    explicit NativeSignalPipe(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace p2pdc::detail
