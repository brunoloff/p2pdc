#include "native_ice_config.hpp"
#include "../core/ice_config_validation.hpp"
#include "native_backend_features.hpp"

#include <rtc/global.hpp>

#include <stdexcept>

namespace p2pdc::detail {
namespace {
rtc::IceServer::RelayType relay_type(TurnTransport transport, unsigned int features) {
    switch (transport) {
    case TurnTransport::Udp:
        return rtc::IceServer::RelayType::TurnUdp;
    case TurnTransport::Tcp:
        return rtc::IceServer::RelayType::TurnTcp;
    case TurnTransport::Tls:
        if ((features & native_turn_tls) == 0)
            throw std::invalid_argument("native TURN over TLS requires the patched ICE backend");
        return rtc::IceServer::RelayType::TurnTls;
    }
    throw std::invalid_argument("unsupported TURN transport");
}

void add_server(rtc::Configuration &config, const StunServer &server, unsigned int) {
    config.iceServers.emplace_back(server.host, server.port);
}

void add_server(rtc::Configuration &config, const TurnServer &server, unsigned int features) {
    config.iceServers.emplace_back(server.host, server.port, server.username, server.password,
                                   relay_type(server.transport, features));
}
} // namespace

NativeIceConfig make_native_ice_config(const IceConfig &config) {
    return make_native_ice_config(config, rtc::P2pdcBackendFeatures());
}

NativeIceConfig make_native_ice_config(const IceConfig &config, unsigned int features) {
    validate_ice_config(config);
    NativeIceConfig result{};
    switch (config.relay_policy) {
    case RelayPolicy::Automatic:
        result.policy = NativeRelayPolicy::Automatic;
        break;
    case RelayPolicy::RequireLocalRelay:
        result.policy = NativeRelayPolicy::RequireLocalRelay;
        break;
    default:
        throw std::invalid_argument("unsupported relay policy");
    }
    for (const auto &server : config.servers) {
        std::visit([&](const auto &value) { add_server(result.rtc, value, features); }, server);
    }
    return result;
}
} // namespace p2pdc::detail
