# 0.2.0

p2pdc discovers peers through OpenDHT and opens authenticated, reliable,
ordered binary WebRTC connections, with one C++20 API for native programs and
browser WebAssembly. This is the first release of the public source, under the
MIT License; dependencies keep their own licenses.

## Changes since 0.1.0-alpha.1

- **Faster reliable sends.** The reliable channel moves 64 frames per executor
  task instead of 4. Applications whose event loop runs one pass of tasks at a
  time were capped near 4 KiB per pass; PeerChalk's PDF transfers in its tests
  took a third of the time.
- **Receivers that fall behind hold the sender back.** A receiver now takes
  messages from the transport only while the application's event queue has
  room, so SCTP flow control slows the peer instead of the connection failing.
- **Unreliable datagrams** on connections, for data where the newest value wins.
- **Persistent incoming listener**, and a shared-text example.
- **Security audit fixes**, recorded in the audit notes, including domain-
  separated application signatures, per-lease identities, relay-only candidate
  filtering and bounded DHT and proxy record handling.
- **Builds from pinned sources** for every dependency, with CI for Linux and
  WebAssembly. macOS 13 or later and Windows (MinGW) builds are tested through
  PeerChalk's release builds.
- A crash when a connection closed during receive is fixed.

## Notes

The protocol and API are still experimental: pin the same p2pdc revision on all
peers. Applications supply their own OpenDHT bootstrap peers, an HTTPS OpenDHT
proxy for browsers, and STUN/TURN servers when needed. Android, iOS and Safari
are not yet validated.

See the [build instructions](README.md#native-build-and-tests), the
[public API guide](docs/PUBLIC_API.md) and the
[WebAssembly instructions](docs/WEBASSEMBLY_INTERNALS.md).
