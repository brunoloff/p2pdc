# Unreleased

## Changes since 0.3.0

- **Native nodes meet through Nostr relays and MQTT brokers too.** Besides joining
  the DHT, a native node now uses the Nostr relays and MQTT 5 brokers of its backend
  list, all at once with the same rules as browsers, so two people still meet when
  one of them is on a network that blocks the DHT. A node keeps one connection per
  server, shared by all its stores. OpenDHT proxy entries remain browser-only;
  native nodes do not use PeerJS servers yet.
- New dependencies for native builds: libdatachannel built with WebSocket support,
  libsecp256k1 0.7.0 (Nostr signatures) and nlohmann/json 3.12.0; `deps/` builds
  them. `-DP2PDC_ENABLE_NETWORK_BACKENDS=OFF` builds without them.
- **TLS on Windows:** libdatachannel does not check server certificates on Windows,
  so these connections are not authenticated there (see
  [discovery backends](docs/DISCOVERY_BACKENDS.md#in-native-nodes)).

## Breaking change: browser backends are now discovery backends

The backends are no longer browser-only, so their names changed:

| 0.3.0 | now |
|---|---|
| `p2pdc/browser_backends.h` | `p2pdc/discovery_backends.h` |
| `DiscoveryConfig::browser_backends` | `DiscoveryConfig::discovery_backends` |
| `BrowserBackend` | `DiscoveryBackend` |
| `parse_browser_backend()` | `parse_discovery_backend()` |
| `valid_browser_backend()` | `valid_discovery_backend()` |
| `max_browser_backends`, `max_browser_proxies` | `max_discovery_backends`, `max_opendht_proxies` |

Native nodes used to ignore the list; they now check it, and `Node::create` throws
`std::invalid_argument` for more than eight entries or an invalid one.

# 0.3.0

p2pdc discovers peers through OpenDHT and opens authenticated, reliable,
ordered binary WebRTC connections, with one C++20 API for native programs and
browser WebAssembly, under the MIT License.

## Changes since 0.2.0

- **Browsers find peers through more kinds of servers.** Besides OpenDHT
  proxies, a browser node can now meet peers through Nostr relays, MQTT 5
  brokers and PeerJS servers, all over WebSocket. Networks that block the DHT
  or streaming HTTP often still allow these.
- **Several meeting places at once.** `DiscoveryConfig::browser_backends` lists
  up to eight backends, used together: values go to all of them, lookups merge
  their answers, and a backend that fails is retried while the others carry on.
  Each entry has a one-line text form (`nostr wss://relay.example`) that
  `parse_browser_backend()` reads and `to_string()` writes, for settings.
- **OpenDHT proxy failover.** One `opendht` entry can list several proxies into
  the same DHT; the browser moves to the next when one fails.
- Stores on a page share one connection per server.

See [discovery backends](docs/DISCOVERY_BACKENDS.md) for how each backend
works and which public servers were found to work.

## Breaking change

`DiscoveryConfig::browser_proxy_urls` is replaced by `browser_backends`. Wrap
the old URLs in one `p2pdc::OpenDhtProxies{...}` entry to keep the old
behavior.

## Notes

The protocol and API are still experimental: pin the same p2pdc revision on all
peers. Native nodes use the DHT directly and ignore browser backends.
Applications supply their own OpenDHT bootstrap peers, browser backends, and
STUN/TURN servers when needed. Android, iOS and Safari are not yet validated.

See the [build instructions](README.md#native-build-and-tests), the
[public API guide](docs/PUBLIC_API.md) and the
[WebAssembly instructions](docs/WEBASSEMBLY_INTERNALS.md).
