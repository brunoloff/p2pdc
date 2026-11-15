#include "peer_admission.hpp"

#include <sodium.h>

#include <utility>

namespace p2pdc::detail {
PeerAdmissionResult resolve_peer_context(DiscoveryBackend &lease, DiscoveryBackend &subscription,
                                         const Advertisement &peer) {
    if (!lease.healthy() || !subscription.healthy())
        return AdmissionError::Closed;
    auto local = lease.lease_snapshot();
    if (!local)
        return lease.healthy() ? AdmissionError::LocalNotPublished : AdmissionError::Closed;
    auto remote = subscription.peer_snapshot(peer);
    if (!remote)
        return subscription.healthy() ? AdmissionError::UnverifiedPeer : AdmissionError::Closed;
    if (!local->endpoint.identity || !local->endpoint.identity->signing ||
        !local->endpoint.identity->recipient || !local->capability || !remote->capability)
        return AdmissionError::Closed;
    if (local->protocol != remote->protocol || local->capability->size() != capability_size ||
        remote->capability->size() != capability_size ||
        sodium_memcmp(local->capability->data(), remote->capability->data(), capability_size) != 0)
        return AdmissionError::DifferentGroup;
    if (local->endpoint.identity->signing->public_key() == remote->peer.signing_public_key &&
        local->endpoint.fields.instance_id == remote->peer.fields.instance_id)
        return AdmissionError::Self;
    return PeerAdmission{std::move(local->endpoint), std::move(remote->peer),
                         std::move(local->capability), remote->max_lifetime, remote->clock_skew};
}
} // namespace p2pdc::detail
