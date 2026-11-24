#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p2pdc {

struct BootstrapServer {
    // DNS name or bare IP address, without a URI scheme or brackets; at most
    // 253 bytes. The port must be nonzero.
    std::string host;
    std::uint16_t port = 4222;
};

struct NativeBindAddresses {
    // Bare numeric addresses for both families; the backend validates them.
    std::string ipv4;
    std::string ipv6;
};

// Configuration shared by native and browser builds. Native OpenDHT uses the
// bind, network and bootstrap settings. Browser discovery uses an HTTPS proxy
// connected to the desired OpenDHT network; native builds ignore proxy URLs.
// The experimental browser Node currently requires exactly one proxy URL;
// proxy failover is not implemented.
//
// A zero bind port selects an ephemeral port; the native
// backend binds wildcard interfaces unless bind_addresses is supplied. Peers
// must use the same network ID. Zero
// selects the public network; other IDs do not provide authentication. No public
// bootstrap service is assumed. An empty list supplies no bootstrap peers;
// at most 32 servers may be supplied.
// Successful startup does not mean a bootstrap server was reached: hostname
// resolution and connection are asynchronous.
struct DiscoveryConfig {
    std::uint16_t bind_port = 0;
    std::uint32_t network_id = 0;
    std::vector<BootstrapServer> bootstrap_servers;
    std::optional<NativeBindAddresses> bind_addresses;
    // Browser-accessible OpenDHT proxy endpoints, 1 to 8, all reaching the same DHT. The
    // browser uses the first that works, in order, and moves on to the next when one is
    // unreachable, times out or answers 408, 429 or 5xx. URLs require HTTPS, except an HTTP
    // loopback endpoint may be used for testing.
    std::vector<std::string> browser_proxy_urls;
    // Maximum retained connection handles per node, including pending attempts.
    // Shared by all capabilities. Valid range: 1 through 1024.
    std::size_t max_connections = 8;
};

// Source compatibility with the earlier native-only interface.
using NativeDiscoveryConfig = DiscoveryConfig;

} // namespace p2pdc
