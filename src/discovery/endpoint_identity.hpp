#pragma once

#include "p2pdc/advertisement.h"
#include "p2pdc/recipient_crypto.h"

namespace p2pdc::detail {

struct EndpointIdentity {
    std::shared_ptr<const Identity> signing;
    std::shared_ptr<const RecipientKeyPair> recipient;
};

struct AnnouncementEndpoint {
    std::shared_ptr<const EndpointIdentity> identity;
    AdvertisementFields fields;
};

[[nodiscard]] CryptoResult<std::shared_ptr<const EndpointIdentity>> make_endpoint_identity();
[[nodiscard]] AnnouncementEndpoint
make_announcement_endpoint(std::shared_ptr<const EndpointIdentity> identity);

} // namespace p2pdc::detail
