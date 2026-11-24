# Discovery backends

Besides the DHT, nodes find peers, and exchange connection offers, through servers they can
reach over HTTPS or WebSocket. Browsers cannot join the DHT, so for them these servers are the
only way to meet; native nodes join the DHT directly and use the servers as well, so that two
people still meet when one of them is on a network that blocks the DHT. This document
describes how p2pdc lets an application choose those servers, and the four kinds it supports.

## What every backend provides

All of discovery (advertisement leases, discovery watches, mailboxes and incoming
listeners) runs on one interface, the record store (`src/discovery/record_store.hpp`):

- **put(key, value):** keep a small value under a 20-byte key for a while (ten minutes);
- **lookup(key):** every current value under the key, from any number of writers;
- **watch(key):** be told about values put under the key from now on.

Values are opaque to the backend: p2pdc signs, encrypts and checks them itself, and treats
whatever a backend returns as untrusted. A backend must only keep values long enough, hand
them to anyone who asks for the key, and push new ones to watchers.

In the browser the backends are implemented in JavaScript (`src/browser/`), by objects
with three methods, the **record backend** contract:

```js
put(key, value, signal)            // Promise; resolves once stored
get(key, signal, { deduplicateByContent }) // Promise<[{ bytes, expired, id }]>
listen(key, onRecord, signal)      // { ready: Promise<boolean>, done: Promise, close() }
```

`key` is a 20-byte `Uint8Array`; `value` and `bytes` are `Uint8Array`s; `id` is a string that
identifies a value within its backend. Failures reject with a `ProxyError` whose code says
whether trying again, or elsewhere, may help (`isTransientProxyError`).

## The four backends

| Backend | Server | put | lookup | watch |
|---|---|---|---|---|
| OpenDHT proxies | HTTPS proxies into one DHT; failover between them | POST a value | GET the key | streaming GET |
| Nostr relay | one `wss://` relay (NIP-01) | publish a signed event tagged with the key, expiring (NIP-40) | REQ with the tag until EOSE | REQ kept open |
| MQTT broker | one MQTT 5 broker over `wss://` | publish a retained message to a topic of its own under the key, expiring | subscribe and collect the retained messages | subscription kept open |
| PeerJS server | one PeerJS server (`wss://`), which only relays WebRTC signalling between IDs | send to the key's host over a data channel | ask the key's host | ask the key's host to forward new values |

**Nostr:** events are kind 30078 (application data), with a `d` tag naming the value, an
`x` tag holding the key in hex and an `expiration` tag. Each store signs with a throwaway
secp256k1 key (BIP340 Schnorr, `schnorr.mjs`): relays require a signature, while p2pdc's own
signature inside the value is the one peers check.

**MQTT:** each value is a retained message on `p2pdc/v1/<key hex>/<value id>` with a message
expiry interval; a lookup subscribes to `p2pdc/v1/<key hex>/+` and collects what arrives
until the broker goes quiet.

**PeerJS:** the server stores nothing and cannot list who is connected; it relays WebRTC
signalling to an ID while that ID is connected, as PeerJS is meant to be used. So each key
gets a host: the first store that registers the PeerJS ID `p2pdc<key hex>` keeps that key's
values in memory, answers lookups and forwards new values to watchers. Others get `ID-TAKEN`
and, from their own ID `p2pdcc<32 hex>`, open a PeerJS data connection to the host:

- the client creates an `RTCPeerConnection` with an ordered, reliable data channel labelled
  with a fresh connection ID `dc_<32 hex>`, and sends `OFFER` with the payload the PeerJS
  client library sends (`sdp`, `type: "data"`, `connectionId`, `label`, `reliable: true`,
  `serialization: "raw"`); the host answers with `ANSWER` (`sdp`, `type`, `connectionId`) and
  both send each ICE candidate as `CANDIDATE` (`candidate`, `type`, `connectionId`).
  Candidates that arrive before the remote description wait for it;
- ICE uses the PeerJS client's defaults: Google's STUN server and the public TURN relays
  PeerJS runs (`eu-0` and `us-0.turn.peerjs.com`);
- requests, replies and pushed values then travel over the channel as JSON messages
  (`put`, `get`, `watch`, `unwatch`; `ack`, `full`, `values`, `value`), each sent as
  fragments of up to 16 KiB marked `+` (more follows) or `=` (last), up to 2 MiB in all.

A client keeps one data connection per host, shared by every store of the page and closed
after a minute unused; a host accepts up to 128. Once open, a connection no longer needs the
server. When the host leaves (its data connection closes, or the server answers an offer
with `EXPIRE`), its ID becomes free: the next store to need the key registers it and becomes
the host, every store puts its own current values again, and watches subscribe again.
Watches are leases of 90 seconds, renewed every 30. The protocol needs only a WebSocket and
WebRTC data channels, so a native port could use libdatachannel; native nodes use the DHT
and do not use this backend yet. Where WebRTC is missing (as in Node) the backend refuses
every request with a permanent error, and other backends carry on.

## Servers, as tested

Live tests (November 2026) found these differences between public servers:

- **Nostr:** nos.lol, relay.primal.net, nostr.mom and relay.snort.social keep, find and push
  values; relay.damus.io and relay.snort.social sometimes refuse new connections from an
  address that opened many, and relay.nostr.band did not answer. Relays also replay events
  from the second a watch began (NIP-01 `since` is inclusive), so watches may hear a value
  twice; p2pdc deduplicates values anyway.
- **MQTT:** the broker must keep retained messages. test.mosquitto.org and broker.hivemq.com
  do; broker.emqx.io acknowledged puts with reason 0x10 but replayed nothing, so lookups
  there find only values put while watching. A backend cannot tell from the acknowledgment.
- **PeerJS:** the public `wss://0.peerjs.com/peerjs` (key `peerjs`) relays only genuine PeerJS
  signalling and drops anything else. p2pdc sends nothing else, so it works there: two
  clients put, looked up and watched values through it, and a host's leaving handed the key
  to a watcher, which then heard new values. A self-hosted `peerjs-server` works the same
  way. Two native clients did the same through it (eight runs in a row). When a key's ID is
  taken, 0.peerjs.com sometimes closes the second connection before its `ID-TAKEN` reaches
  libdatachannel; native stores take a claim that fails that way to mean the key is hosted
  elsewhere, and claim it again if the server then expires their offer.

Each store keeps one connection per server, shared with every other store of the page (or
native node) that uses the same server (`discovery_backends.mjs`, `network_backends.cpp`):
p2pdc creates a store per space and per connection attempt, and servers limit how many
connections one address may open.

## Several meeting places at once

Each backend entry is a separate meeting place: two nodes meet only through a backend that
both use and can reach. OpenDHT proxies into the same DHT are one meeting place, so one entry
lists them all and the store fails over between them. Separate entries are used **all at
once** (`fan_out_store.mjs`), not one after another, because a node that falls back to
another meeting place would no longer meet the nodes that stayed:

- a put goes to every backend and succeeds when one does;
- a lookup asks every backend and merges the answers; once one has answered it waits a
  little for the others, so a blocked backend does not slow discovery down;
- a watch watches every backend; one that fails is watched again after a backoff while the
  others carry on.

## In native nodes

Native nodes join the DHT themselves and, beside it, use every Nostr relay, MQTT broker and
PeerJS server of their backend list, all at once with the same rules (`src/backends/`).
OpenDHT entries are skipped: they name HTTPS proxies into the DHT that a native node has no
need for.

The protocol layers are ports of the JavaScript ones, one C++ file per module:
`socket_channel.cpp` (`websocket_channel.mjs`), `nostr_event.cpp` and `nostr_relay.cpp`
(`nostr_relay.mjs`), `mqtt_codec.cpp` (`mqtt_codec.mjs`), `mqtt_broker.cpp`
(`mqtt_broker.mjs`), `backend_records.cpp` (`store_limits.mjs`) and `fan_out_store.cpp` with
`fan_out_watch.cpp` (`fan_out_store.mjs`). PeerJS is `peerjs_socket.cpp`, `peerjs_link.cpp`,
`peerjs_endpoint.cpp`, `peerjs_host.cpp` and `peerjs_switchboard*.cpp`, with the same IDs,
signalling and record protocol as the JavaScript, on libdatachannel's `PeerConnection` and
`DataChannel` with the PeerJS client's ICE servers; a browser and a native node can be each
other's key host. `network_backends.cpp` plays the part of
`discovery_backends.mjs`: one client per server and node, shared by every store.

- **Threads.** All clients of a node run on one thread of their own (`backend_loop.cpp`), as
  the JavaScript runs on the page's event loop. Results of the DHT store are passed to that
  thread too, so the fan-out store delivers every callback from it, one at a time.
- **WebSocket and TLS.** Connections use libdatachannel's WebSocket client. On Linux it checks
  certificates against the system's certificate authorities and the server's name
  (`VerifiedTlsTransport`); a test checks that a server with a self-signed certificate is
  refused. The OpenSSL that `deps/` builds for macOS has no certificate store of its own, so
  p2pdc points it at the system bundle, `/etc/ssl/cert.pem`; this has not been tested on a
  Mac. **On Windows, libdatachannel 0.23.2 does not check certificates at all**: someone on
  the path could read, hold back or answer the traffic of these backends. Values stay signed
  and encrypted by p2pdc, so they could not forge or read peers' records, but they could see
  which keys a node uses and keep peers from meeting. Fixing this needs a libdatachannel patch
  that checks certificates on Windows against a bundle the application ships.
- **Signatures.** Nostr events are signed with libsecp256k1's BIP340 Schnorr signatures, and
  their ids hashed with libsodium's SHA-256; the official test vectors are checked.
- **Building.** The backends need libdatachannel built with WebSocket support, libsecp256k1
  and nlohmann/json; `deps/` builds all three. `P2PDC_ENABLE_NETWORK_BACKENDS` (on by default
  for native builds other than Android) turns them off; without them, a native node refuses a
  backend list that names anything but OpenDHT proxies.

## Choosing backends

`DiscoveryConfig::discovery_backends` lists the entries, as a `DiscoveryBackend`: a variant of
`OpenDhtProxies`, `NostrRelay`, `MqttBroker` and `PeerJsServer` (`p2pdc/discovery_backends.h`).
Browser nodes take 1 to 8 entries; native nodes 0 to 8, and `Node::create` throws
`std::invalid_argument` for an invalid one. For settings typed by people, each entry also has
a one-line form that `parse_discovery_backend()` reads and `to_string()` writes:

```text
opendht https://dhtproxy.jami.net https://proxy.example
nostr wss://relay.damus.io
mqtt wss://broker.example/mqtt
peerjs wss://0.peerjs.com/peerjs?key=peerjs
```

Enabling, disabling or switching a backend is adding, removing or replacing an entry. An
application can give browser and native nodes the same list: native nodes skip the OpenDHT
entry, since they join the DHT itself.

## Testing

- Unit tests (`node --test tests/*_test.mjs`) run each backend against a small in-process
  server written for the tests (`tests/support/`): a WebSocket server, a Nostr relay, an
  MQTT 5 broker and a PeerJS server. They cover put, lookup, watch, expiry, limits,
  reconnection and failure codes, and the fan-out store's merging and failure handling.
  The PeerJS server relays, like the public one, only messages shaped as PeerJS signalling,
  and the PeerJS tests run over an in-memory stand-in for `RTCPeerConnection`
  (`tests/support/fake_webrtc.mjs`) that connects only after a full offer, answer and
  candidate exchange.
- With real WebRTC, `tests/peerjs_webrtc_browser_test.mjs` runs put, lookup, watch and host
  takeover in headless Chromium against that server (opt-in:
  `P2PDC_TEST_CHROMIUM=/usr/bin/chromium`).
- BIP340 signing is checked against the official test vectors.
- Live tests (`tests/discovery_backends_live_test.mjs`, opt-in through
  `P2PDC_TEST_NOSTR_RELAYS`, `P2PDC_TEST_MQTT_BROKERS` and `P2PDC_TEST_PEERJS_SERVERS`) run
  put, lookup and watch between two clients against public servers; PeerJS servers are
  tested in headless Chromium, so they also need `P2PDC_TEST_CHROMIUM`.
- End to end, two browser users of PeerChalk, each given one backend only, found each
  other and connected through a Nostr relay, an MQTT broker and a PeerJS server.
- Native tests (`p2pdc_backend_tests`) start the same test servers with node
  (`tests/support/serve_backends.mjs`) and cover the native clients the same way: put, lookup,
  watch, limits, refusals, reconnection, a shared connection, and the fan-out store with fake
  stores. The browser's own clients, run by node, and the native ones read and hear each
  other's values through each server (Nostr and MQTT; the browser's PeerJS client needs
  WebRTC, which node lacks). The PeerJS tests (`peerjs_store_test.cpp`) run native stores
  over real data connections through the strict test server: hosting, lookups and watches on
  and away from the host, takeover when the host leaves, a refused server key, and a server
  that closes taken IDs without saying so. With `P2PDC_TEST_CHROMIUM` set, a browser client in
  headless Chromium and a native store host keys for each other, each reading, writing and
  watching through the other. The tests also run under AddressSanitizer.
- `p2pdc_network_node_tests` runs two native nodes, each alone in a DHT of its own, that find
  each other, connect and echo through a Nostr relay, through an MQTT broker, through a PeerJS
  server, and through a broker beside an unreachable relay; without a shared backend they do
  not meet.
- Native live tests (`p2pdc_backend_live_tests`, opt-in through the same variables as the
  JavaScript ones) run put, lookup and watch between two native clients against public
  servers.
