#include "browser_ice_config.hpp"
#include "../core/ice_config_validation.hpp"

#include <stdexcept>

namespace p2pdc::detail {
namespace {
std::string address(const std::string &host, std::uint16_t port) {
    return (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" +
           std::to_string(port);
}
BrowserIceServer convert(const StunServer &server) {
    return {"stun:" + address(server.host, server.port), {}, {}};
}
BrowserIceServer convert(const TurnServer &server) {
    std::string scheme, suffix;
    switch (server.transport) {
    case TurnTransport::Udp:
        scheme = "turn:";
        suffix = "?transport=udp";
        break;
    case TurnTransport::Tcp:
        scheme = "turn:";
        suffix = "?transport=tcp";
        break;
    case TurnTransport::Tls:
        scheme = "turns:";
        suffix = "?transport=tcp";
        break;
    default:
        throw std::invalid_argument("unsupported TURN transport");
    }
    return {scheme + address(server.host, server.port) + suffix, server.username, server.password};
}
} // namespace
std::vector<BrowserIceServer> browser_ice_servers(const IceConfig &config) {
    validate_ice_config(config);
    std::vector<BrowserIceServer> result;
    result.reserve(config.servers.size());
    for (const auto &server : config.servers) {
        result.push_back(std::visit([](const auto &value) { return convert(value); }, server));
    }
    return result;
}
} // namespace p2pdc::detail
