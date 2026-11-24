#pragma once

#include "p2pdc/browser_backends.h"

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
// bind, network and bootstrap settings. Browser discovery uses the browser
// backends (p2pdc/browser_backends.h), all at once; native builds ignore them.
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
    // Where browser nodes meet, 1 to 8 backends; a browser node uses all of them at once.
    std::vector<BrowserBackend> browser_backends;
    // Maximum retained connection handles per node, including pending attempts.
    // Shared by all capabilities. Valid range: 1 through 1024.
    std::size_t max_connections = 8;
};

// Source compatibility with the earlier native-only interface.
using NativeDiscoveryConfig = DiscoveryConfig;

} // namespace p2pdc
