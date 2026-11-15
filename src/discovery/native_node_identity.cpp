#include "p2pdc/node.h"

#include "native_node_state.hpp"

#include <stdexcept>
#include <utility>

namespace p2pdc {

std::variant<std::unique_ptr<Node>, CryptoError, NodeIdentityError>
Node::create_with_identity(DiscoveryConfig config, Executor executor, ByteView signing_seed,
                           ByteView recipient_secret) {
    if (!executor)
        throw std::invalid_argument("node requires an executor");
#ifdef P2PDC_HAVE_DISCOVERY
    auto signing = Identity::import_seed(signing_seed);
    if (auto *error = std::get_if<CryptoError>(&signing))
        return *error;
    auto recipient = RecipientKeyPair::import_secret_key(recipient_secret);
    if (auto *error = std::get_if<CryptoError>(&recipient))
        return *error;
    auto endpoint = std::make_shared<detail::EndpointIdentity>();
    endpoint->signing = std::make_shared<Identity>(std::move(std::get<Identity>(signing)));
    endpoint->recipient =
        std::make_shared<RecipientKeyPair>(std::move(std::get<RecipientKeyPair>(recipient)));
    auto node = create(std::move(config), std::move(executor));
    node->state_->endpoint = std::move(endpoint);
    return node;
#else
    (void)config;
    (void)signing_seed;
    (void)recipient_secret;
    return NodeIdentityError::Unsupported;
#endif
}

std::optional<NodePublicIdentity> Node::identity() const noexcept {
#ifdef P2PDC_HAVE_DISCOVERY
    const auto &endpoint = state_->endpoint;
    if (endpoint)
        return NodePublicIdentity{endpoint->signing->public_key(),
                                  endpoint->recipient->public_key()};
#endif
    return std::nullopt;
}

} // namespace p2pdc
