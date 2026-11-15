#include "endpoint_identity.hpp"

#include <sodium.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
template <class Array> void random_nonzero(Array &value) {
    do {
        randombytes_buf(value.data(), value.size());
    } while (std::all_of(value.begin(), value.end(),
                         [](std::byte byte) { return byte == std::byte{}; }));
}
} // namespace

CryptoResult<std::shared_ptr<const EndpointIdentity>> make_endpoint_identity() {
    auto signing = Identity::generate();
    if (auto *error = std::get_if<CryptoError>(&signing)) {
        return *error;
    }
    auto recipient = RecipientKeyPair::generate();
    if (auto *error = std::get_if<CryptoError>(&recipient)) {
        return *error;
    }
    auto endpoint = std::make_shared<EndpointIdentity>();
    endpoint->signing = std::make_shared<Identity>(std::move(std::get<Identity>(signing)));
    endpoint->recipient =
        std::make_shared<RecipientKeyPair>(std::move(std::get<RecipientKeyPair>(recipient)));
    return std::shared_ptr<const EndpointIdentity>(std::move(endpoint));
}

AnnouncementEndpoint make_announcement_endpoint(std::shared_ptr<const EndpointIdentity> identity) {
    if (!identity || !identity->signing || !identity->recipient) {
        throw std::invalid_argument("announcement requires complete endpoint identity");
    }
    AdvertisementFields fields{};
    fields.recipient_public_key = identity->recipient->public_key();
    random_nonzero(fields.instance_id);
    random_nonzero(fields.signaling_inbox);
    return {std::move(identity), fields};
}
} // namespace p2pdc::detail
