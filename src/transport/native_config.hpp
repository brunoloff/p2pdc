#pragma once

#include "native_backend_features.hpp"
#include "native_transport.hpp"

#include <algorithm>
#include <rtc/global.hpp>
#include <stdexcept>

namespace p2pdc::detail {

inline std::optional<CandidatePair> usable_route_pair(bool connected, bool channel_open,
                                                      std::optional<CandidatePair> pair) {
    if (!connected || !channel_open) {
        return std::nullopt;
    }
    return pair;
}

inline CandidateKind candidate_kind(rtc::Candidate::Type type) {
    switch (type) {
    case rtc::Candidate::Type::Host:
        return CandidateKind::Host;
    case rtc::Candidate::Type::ServerReflexive:
        return CandidateKind::ServerReflexive;
    case rtc::Candidate::Type::PeerReflexive:
        return CandidateKind::PeerReflexive;
    case rtc::Candidate::Type::Relayed:
        return CandidateKind::Relayed;
    default:
        return CandidateKind::Unknown;
    }
}

inline rtc::Configuration prepare_native_config(rtc::Configuration config,
                                                NativeRelayPolicy policy) {
    if (policy != NativeRelayPolicy::Automatic && policy != NativeRelayPolicy::RequireLocalRelay) {
        throw std::invalid_argument("Unknown relay policy");
    }
    const bool uses_turn = std::any_of(
        config.iceServers.begin(), config.iceServers.end(),
        [](const rtc::IceServer &server) { return server.type == rtc::IceServer::Type::Turn; });
    if (!supports_native_backend(rtc::P2pdcBackendFeatures(),
                                 policy == NativeRelayPolicy::RequireLocalRelay, uses_turn)) {
        throw std::runtime_error("libdatachannel lacks required p2pdc backend fixes");
    }
    if (policy == NativeRelayPolicy::RequireLocalRelay) {
        const bool turn = std::any_of(
            config.iceServers.begin(), config.iceServers.end(), [](const rtc::IceServer &server) {
                return server.type == rtc::IceServer::Type::Turn && !server.hostname.empty();
            });
        if (!turn) {
            throw std::invalid_argument("RequireLocalRelay needs a configured TURN server");
        }
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }
    config.disableAutoNegotiation = true;
    config.disableAutoGathering = false;
    return config;
}

} // namespace p2pdc::detail
