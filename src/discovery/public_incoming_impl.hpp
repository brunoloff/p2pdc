#pragma once

#include "../core/timer_producer.hpp"
#include "native_public_incoming.hpp"
#include "pending_offers.hpp"

namespace p2pdc::detail {
class PublicIncoming final : public IncomingBackend,
                             public std::enable_shared_from_this<PublicIncoming> {
  public:
    PublicIncoming(RecordStore store, IncomingContext initial,
                   std::function<std::optional<IncomingContext>()> context,
                   std::weak_ptr<DiscoveryBackend> lease,
                   std::weak_ptr<DiscoveryBackend> subscription, Executor executor,
                   CallbackGate gate, Scheduler scheduler, IncomingOptions options,
                   IncomingHandlers handlers, std::shared_ptr<IncomingOfferReplay> replay);
    void initialize();
    [[nodiscard]] std::optional<IncomingError> start() override;
    void close() override;
    [[nodiscard]] IncomingState state() const override;
    [[nodiscard]] bool signaling_closed() const override;
    [[nodiscard]] std::optional<Advertisement> peer() const override;
    [[nodiscard]] std::optional<IncomingRequestId> request_id() const override;
    [[nodiscard]] std::optional<PendingIncoming> peek() const override;
    [[nodiscard]] std::optional<PendingIncoming> take() override;
    [[nodiscard]] std::optional<PendingIncoming>
    peek_request(std::optional<std::uint64_t> id) const override;
    [[nodiscard]] std::optional<PendingIncoming>
    take_request(std::optional<std::uint64_t> id) override;
    void reject_request(std::optional<std::uint64_t> id) override;
    void reject_taken(const PendingIncoming &request) override;

  private:
    void receive(IncomingOffer offer) noexcept;
    void deliver(IncomingRequestId id);
    void fail(IncomingError error) noexcept;
    void tick();
    void cleanup();
    void reject_pending(IncomingRequestId id) noexcept;
    void reject_offer(const IncomingOffer &offer) noexcept;
    void send_reject(const IncomingOffer &offer, const AnnouncementEndpoint &local,
                     const std::shared_ptr<const Bytes> &capability) noexcept;
    void report_overflow();
    void check_cancel(const Bytes &wire) noexcept;
    void lookup_cancel() noexcept;
    void end_request(IncomingRequestId id, IncomingError error);
    [[nodiscard]] const PendingOffers::Entry *pending() const;
    void clear_pending();
    RecordStore store_;
    IncomingContext initial_;
    std::function<std::optional<IncomingContext>()> context_;
    std::weak_ptr<DiscoveryBackend> lease_, subscription_;
    Executor executor_;
    CallbackGate gate_;
    Scheduler scheduler_;
    IncomingOptions options_;
    IncomingHandlers handlers_;
    std::shared_ptr<IncomingOfferReplay> replay_;
    SigningPublicKey signing_key_;
    RecipientBoxPublicKey box_key_;
    std::unique_ptr<TimerProducer> timer_;
    std::unique_ptr<NativeIncomingListener> listener_;
    PendingOffers offers_;
    std::optional<PendingOffers::Id> pending_id_;
    IncomingState state_ = IncomingState::Listening;
    bool overflow_queued_ = false;
};
} // namespace p2pdc::detail
