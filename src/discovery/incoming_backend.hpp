#pragma once

#include "discovery_backend.hpp"
#include "incoming_offer_resolver.hpp"
#include "p2pdc/incoming.h"

namespace p2pdc::detail {
struct PendingIncoming {
    IncomingOffer offer;
    AnnouncementEndpoint endpoint;
    std::weak_ptr<DiscoveryBackend> lease;
    std::weak_ptr<DiscoveryBackend> subscription;
    SigningPublicKey signing_key;
    RecipientBoxPublicKey box_key;
    // Kept so a taken request can still be rejected after the listener released it.
    std::shared_ptr<const Bytes> capability;
};
class IncomingBackend {
  public:
    virtual ~IncomingBackend() = default;
    virtual void close() = 0;
    [[nodiscard]] virtual IncomingState state() const = 0;
    [[nodiscard]] virtual bool signaling_closed() const = 0;
    [[nodiscard]] virtual std::optional<Advertisement> peer() const = 0;
    [[nodiscard]] virtual std::optional<IncomingRequestId> request_id() const = 0;
    [[nodiscard]] virtual std::optional<PendingIncoming> peek() const = 0;
    [[nodiscard]] virtual std::optional<PendingIncoming> take() = 0;
    // A null ID selects the legacy single-shot request. Implementations must
    // leave other requests untouched when an explicit ID is used.
    [[nodiscard]] virtual std::optional<PendingIncoming>
    peek_request(std::optional<std::uint64_t> id) const = 0;
    [[nodiscard]] virtual std::optional<PendingIncoming>
    take_request(std::optional<std::uint64_t> id) = 0;
    virtual void reject_request(std::optional<std::uint64_t> id) = 0;
    // Best-effort rejection of a request already removed with take_request,
    // for when local connection setup fails after consent was consumed.
    virtual void reject_taken(const PendingIncoming &request) = 0;
    [[nodiscard]] virtual std::optional<IncomingError> start() = 0;
};
} // namespace p2pdc::detail
