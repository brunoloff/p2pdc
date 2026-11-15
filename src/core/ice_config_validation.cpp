#include "ice_config_validation.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace p2pdc::detail {
namespace {
void validate_address(const std::string &host, std::uint16_t port) {
    if (host.empty() || host.size() > 253 || !port ||
        std::any_of(host.begin(), host.end(),
                    [](unsigned char c) {
                        return std::isspace(c) || std::iscntrl(c) || c == '/' || c == '?' ||
                               c == '#' || c == '@' || c == '[' || c == ']';
                    }) ||
        std::count(host.begin(), host.end(), ':') == 1 || host.find(":::") != std::string::npos)
        throw std::invalid_argument("ICE server needs a valid host and nonzero port");
}
void validate_server(const StunServer &server) { validate_address(server.host, server.port); }
void validate_server(const TurnServer &server) {
    validate_address(server.host, server.port);
    if (server.username.empty() != server.password.empty() || server.username.size() > 1024 ||
        server.password.size() > 1024 || server.username.find('\0') != std::string::npos ||
        server.password.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid TURN credentials");
    if (server.transport != TurnTransport::Udp && server.transport != TurnTransport::Tcp &&
        server.transport != TurnTransport::Tls)
        throw std::invalid_argument("unsupported TURN transport");
}
} // namespace
void validate_ice_config(const IceConfig &config) {
    if (config.servers.size() > 16 || (config.relay_policy != RelayPolicy::Automatic &&
                                       config.relay_policy != RelayPolicy::RequireLocalRelay))
        throw std::invalid_argument("invalid ICE configuration");
    bool turn = false;
    for (const auto &server : config.servers) {
        turn = turn || std::holds_alternative<TurnServer>(server);
        std::visit([](const auto &value) { validate_server(value); }, server);
    }
    if (config.relay_policy == RelayPolicy::RequireLocalRelay && !turn)
        throw std::invalid_argument("RequireLocalRelay needs a TURN server");
}
} // namespace p2pdc::detail
