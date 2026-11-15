#include "native_runner_start.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace p2pdc::detail {
namespace {
void validate_bootstrap(const BootstrapServer &server) {
    if (server.host.empty() || server.host.size() > 253 || server.port == 0 ||
        std::any_of(server.host.begin(), server.host.end(), [](unsigned char value) {
            return std::isspace(value) || std::iscntrl(value) || value == '/' || value == '?' ||
                   value == '#' || value == '@' || value == '[' || value == ']';
        })) {
        throw std::invalid_argument("bootstrap server requires a valid host and nonzero port");
    }
    const auto colons = std::count(server.host.begin(), server.host.end(), ':');
    if (colons == 1 || server.host.find(":::") != std::string::npos) {
        throw std::invalid_argument("bootstrap host must not include a port or URI scheme");
    }
}
} // namespace

RunnerStart make_native_runner_start(NativeDiscoveryConfig config) {
    if (config.bootstrap_servers.size() > 32) {
        throw std::invalid_argument("at most 32 bootstrap servers are supported");
    }
    for (const auto &server : config.bootstrap_servers) {
        validate_bootstrap(server);
    }
    if (config.bind_addresses) {
        validate_bootstrap({config.bind_addresses->ipv4, 1});
        validate_bootstrap({config.bind_addresses->ipv6, 1});
    }
    return [config = std::move(config)](dht::DhtRunner &runner) {
        dht::DhtRunner::Config backend;
        backend.dht_config.node_config.network = config.network_id;
        backend.peer_discovery = false;
        backend.peer_publish = false;
        if (config.bind_addresses) {
            const auto port = std::to_string(config.bind_port);
            runner.run(config.bind_addresses->ipv4.c_str(), config.bind_addresses->ipv6.c_str(),
                       port.c_str(), backend);
        } else {
            runner.run(config.bind_port, backend);
        }
        for (const auto &server : config.bootstrap_servers) {
            runner.bootstrap(server.host, std::to_string(server.port));
        }
    };
}
} // namespace p2pdc::detail
