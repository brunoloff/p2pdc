#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace p2pdc {

enum class RelayPolicy : std::uint8_t { Automatic, RequireLocalRelay };
enum class TurnTransport : std::uint8_t { Udp, Tcp, Tls };

struct StunServer {
    // DNS name or bare IP address; do not include a URI scheme or brackets.
    std::string host;
    std::uint16_t port = 3478;
};

struct TurnServer {
    // DNS name or bare IP address; do not include a URI scheme or brackets.
    std::string host;
    // Set explicitly to 5349 when using the conventional TURN-over-TLS port.
    std::uint16_t port = 3478;
    std::string username;
    std::string password;
    TurnTransport transport = TurnTransport::Udp;
};

using IceServer = std::variant<StunServer, TurnServer>;

// Credentials are supplied per connection attempt and copied into backend
// configuration. They are ordinary strings: the caller owns their protection.
// Tls means TURN over TLS, not TLS for the data channel. The current native
// backend requires the patched libnice TLS implementation and rejects Tls
// when that implementation is not loaded. Browser support and actual relay
// use must be checked separately.
// At most 16 servers are allowed; host names are at most 253 bytes and each
// TURN credential is at most 1024 bytes. Hosts may contain only letters,
// digits, '.', '-', '_' and ':' (bare IPv6; zone identifiers are rejected).
// These limits apply on every target.
struct IceConfig {
    std::vector<IceServer> servers;
    RelayPolicy relay_policy = RelayPolicy::Automatic;
};
} // namespace p2pdc
