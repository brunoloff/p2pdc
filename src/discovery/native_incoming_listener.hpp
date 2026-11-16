#pragma once

#include "incoming_offer_resolver.hpp"
#include "native_mailbox.hpp"
#include "native_public_watch.hpp"

namespace p2pdc::detail {
struct IncomingContext {
    LeaseSnapshot local;
    PeerCollection remote;
};
enum class IncomingListenerError {
    Context,
    Clock,
    ResourceLimit,
    Crypto,
    Mailbox,
    Catchup,
    Callback
};
struct IncomingListenerCallbacks {
    std::function<void(IncomingOffer)> on_offer;
    std::function<void(IncomingListenerError)> on_error;
    // If provided, false prevents consuming additional offers while the owner
    // is unable to present them; a later listener can catch them up from DHT.
    std::function<bool()> can_offer{};
    // All untrusted records, including while offers are being accepted; owner verifies them.
    std::function<void(const Bytes &)> on_pending_record{};
};

// Internal owner-executor component. Callbacks and context/clock providers must
// not throw, reenter or destroy this object. Offers establish identity only;
// callers must obtain consent and revalidate both handles before starting an
// attempt. A bounded queue retries authenticated offers from unknown peers as
// discovery catches up; discovery and delivery remain best effort.
// Retention reserves against the shared budget while the mailbox delivery
// reservation is still live, so admission requires temporary extra headroom.
// The owner must poll, close before stopping the runner, and bound retained
// requests and deferred callbacks separately and enforce one listener per lease.
class NativeIncomingListener {
  public:
    static std::variant<std::unique_ptr<NativeIncomingListener>, StoreError>
    create(RecordStore store, IncomingContext initial,
           std::function<std::optional<IncomingContext>()> context,
           std::function<std::uint64_t()> now, Executor executor, MemoryBudget budget,
           IncomingListenerCallbacks callbacks, std::shared_ptr<IncomingOfferReplay> replay = {});
    ~NativeIncomingListener();
    NativeIncomingListener(const NativeIncomingListener &) = delete;
    NativeIncomingListener &operator=(const NativeIncomingListener &) = delete;
    [[nodiscard]] bool poll();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::size_t pending_offers() const;
    void close();

  private:
    struct State;
    explicit NativeIncomingListener(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
} // namespace p2pdc::detail
