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
