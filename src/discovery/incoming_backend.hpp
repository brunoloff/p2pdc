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
};
class IncomingBackend {
  public:
    virtual ~IncomingBackend() = default;
    virtual void close() = 0;
    [[nodiscard]] virtual IncomingState state() const = 0;
    [[nodiscard]] virtual bool signaling_closed() const = 0;
    [[nodiscard]] virtual std::optional<Advertisement> peer() const = 0;
    [[nodiscard]] virtual std::optional<PendingIncoming> peek() const = 0;
    [[nodiscard]] virtual std::optional<PendingIncoming> take() = 0;
    [[nodiscard]] virtual std::optional<IncomingError> start() = 0;
};
} // namespace p2pdc::detail
