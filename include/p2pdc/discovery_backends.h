#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace p2pdc {

// Where nodes meet: besides the DHT, nodes find peers, and exchange connection offers, through
// servers they reach over HTTPS or WebSocket (see docs/DISCOVERY_BACKENDS.md). Each backend is
// a separate meeting place; a node uses all of its backends at once, and two nodes meet
// through any backend both use and can reach. Browsers cannot join the DHT, so they reach it
// through OpenDHT proxies; native nodes join the DHT themselves and skip OpenDHT entries.
//
// URLs use https:// (OpenDHT proxies) or wss:// (the others), without credentials or
// fragment; http:// and ws:// are accepted for localhost, 127.0.0.1 and [::1], for tests.

// OpenDHT proxies into the same DHT, 1 to 8, used in order: the next takes over while one is
// unreachable, times out or answers 408, 429 or 5xx.
struct OpenDhtProxies {
    std::vector<std::string> urls;
    bool operator==(const OpenDhtProxies &) const = default;
};
// A Nostr relay (NIP-01): values are expiring events signed with a throwaway key.
struct NostrRelay {
    std::string url;
    bool operator==(const NostrRelay &) const = default;
};
// An MQTT 5 broker over WebSocket: values are retained, expiring messages.
struct MqttBroker {
    std::string url;
    bool operator==(const MqttBroker &) const = default;
};
// A server running peers/peerjs-server, which relays WebRTC signalling between connected IDs;
// key is the server's API key. Nodes reach each key's host over a data connection set up
// through it, so the public wss://0.peerjs.com/peerjs (key "peerjs") serves.
struct PeerJsServer {
    std::string url;
    std::string key = "peerjs";
    bool operator==(const PeerJsServer &) const = default;
};
using DiscoveryBackend = std::variant<OpenDhtProxies, NostrRelay, MqttBroker, PeerJsServer>;

inline constexpr std::size_t max_discovery_backends = 8;
inline constexpr std::size_t max_opendht_proxies = 8;

// The one-line form for settings: "opendht URL [URL...]", "nostr URL", "mqtt URL" or
// "peerjs URL [KEY]", words separated by spaces. Returns nullopt for anything else, including
// URLs this library would refuse.
[[nodiscard]] std::optional<DiscoveryBackend> parse_discovery_backend(std::string_view text);
// The one-line form that parse_discovery_backend() reads back.
[[nodiscard]] std::string to_string(const DiscoveryBackend &backend);
// Whether the backend is one a browser node accepts (see the URL rules above).
[[nodiscard]] bool valid_discovery_backend(const DiscoveryBackend &backend);

} // namespace p2pdc
