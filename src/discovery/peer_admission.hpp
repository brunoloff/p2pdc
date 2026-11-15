#pragma once

#include "discovery_backend.hpp"

namespace p2pdc::detail {
enum class AdmissionError {
    NodeNotReady,
    ForeignHandle,
    Closed,
    LocalNotPublished,
    UnverifiedPeer,
    DifferentGroup,
    Self
};

struct PeerAdmission {
    AnnouncementEndpoint local;
    Advertisement remote;
    std::shared_ptr<const Bytes> capability;
    std::uint64_t remote_max_lifetime;
    std::uint64_t remote_clock_skew;
};

using PeerAdmissionResult = std::variant<PeerAdmission, AdmissionError>;

// The caller checks ownership of both handles and revalidates after any delay.
[[nodiscard]] PeerAdmissionResult resolve_peer_context(DiscoveryBackend &lease,
                                                       DiscoveryBackend &subscription,
                                                       const Advertisement &peer);
} // namespace p2pdc::detail
