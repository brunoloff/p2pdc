# Public C++ API guide

This guide describes the experimental `0.3.0` API. Names, behavior,
and wire formats may change. Pin the same library revision on all peers. The
API is C++20 and is shared by Linux and WebAssembly consumers. For supported
platforms and test results, see [alpha status](ALPHA_STATUS.md).

The library discovers peers and opens authenticated, reliable, ordered binary
connections, which can also carry unreliable datagrams. It does not decide whom to trust or synchronize a document. The
application supplies those policies, its discovery infrastructure, and any
STUN/TURN service it needs.

For a complete working program, read [the echo example](../examples/echo/README.md)
and its [implementation](../examples/echo/echo_peer.cpp). The same example
source is used for native and WebAssembly builds. The snippets below illustrate
individual steps; they are not a complete program.

## Add the library

Configure the target's dependencies as described in the [build instructions](../README.md),
then link the source-tree target:

```cmake
set(P2PDC_BUILD_SHARED_TEXT OFF CACHE BOOL "" FORCE) # if you do not need the example
add_subdirectory(path/to/p2p-discovery-connect)
target_link_libraries(my_app PRIVATE p2pdc::p2pdc)
```

There is currently no installed CMake package. The optional shared-text example
is on by default and requires Qt and Yrs; the library itself needs neither.
Include `p2pdc/node.h` and `p2pdc/runtime.h` for the basic flow.

## The objects and their lifetime

| Object | Purpose | Keep it alive until |
| --- | --- | --- |
| `Runtime` | Default executor and timer scheduler. | All nodes have shut down and their handles are released. |
| `Node` | Owns local discovery and creates connections. | Its asynchronous shutdown callback has completed. |
| `Subscription` | Watches a capability for peer advertisements. | Discovery and initial signaling no longer need it. |
| `Lease` | Publishes and refreshes this node's advertisement. | Discovery and initial signaling no longer need it. |
| `IncomingListener` | Receives incoming connection requests. | Closed; in the default single-shot mode it also stops after acceptance. |
| `Connection` | Sends and receives messages with one peer. | The application closes or releases it. |

Keep discovery handles alive through initial signaling admission. Once signaling
starts, a connection owns its bindings independently. Closing a lease stops
refreshing it; previously published records can remain until expiry.

## Execution and shutdown

`Runtime` provides a serial executor and a scheduler. On native platforms it
uses one worker thread; in WebAssembly it uses the browser event loop. Post
node operations to its executor and handle their callbacks there. Dispatch UI
updates to your UI thread. Browser code must run on the browser event-loop
thread and must not block it.

```cpp
p2pdc::Runtime runtime;
auto executor = runtime.executor();
auto scheduler = runtime.scheduler();
executor([&] {
    // Create and use the node here; retained objects must outlive this task.
});
```

Callbacks and tasks must not throw or block the executor. The executor can
throw if posting fails, including allocation failure. If you provide your own
executor, it must accept backend submissions safely, post asynchronously,
serialize callbacks in FIFO order, and not block or reenter the node. A custom
scheduler must satisfy the cancellation and lifetime contract in
[`scheduler.h`](../include/p2pdc/scheduler.h).

Call `node->shutdown(callback)` on the executor and retain the node, runtime,
scheduler, and relevant captures until the callback completes. It returns
`false` if shutdown has already begun. Dropping a node requests cleanup but
does not wait for it. Shutdown drains the node's admitted work, but not
process-wide WebRTC cleanup. `healthy()` and `retry_callbacks()` exist for
exceptional callback-submission failures; see the contract in
[`node.h`](../include/p2pdc/node.h).

## Configure discovery and identity

Each group shares a **32-byte high-entropy secret capability**. Generate one
with `generate_discovery_capability()` and check its `CryptoResult`; exchange
it privately. A password or an all-zero example value is not a suitable
capability. Protect and wipe caller-owned copies. Possession permits group
discovery; it does not authorize a particular peer.

```cpp
p2pdc::DiscoveryConfig config;
config.bootstrap_servers.push_back({"bootstrap.example.org", 4222});
config.discovery_backends.push_back(p2pdc::OpenDhtProxies{{"https://proxy.example.org"}});
auto node = p2pdc::Node::create(config, executor);
```

Replace these illustrative addresses with services you operate or are
authorized to use. Discovery backends are used all at once: OpenDHT proxies
(with failover between proxies into the same DHT), Nostr relays, MQTT 5 brokers
and PeerJS servers; `parse_discovery_backend()` reads the one-line form for
settings (see [DISCOVERY_BACKENDS.md](DISCOVERY_BACKENDS.md)). WebAssembly takes
1 to 8 backends. Native nodes take 0 to 8: they join the DHT through the
bootstrap settings, skip OpenDHT proxy entries, and use Nostr relays, MQTT brokers
and PeerJS servers beside the DHT; `Node::create` throws
`std::invalid_argument` for an invalid entry.
HTTP and WS loopback addresses are allowed for tests.
Peers must use the same DHT network ID. The library does not select public
servers for you.

`Node::create` uses an ephemeral identity, generated when needed. To retain an
identity across sessions, `Node::create_with_identity` takes an Ed25519 signing
seed and X25519 recipient secret and returns a variant containing either a
node or an error. Store these secrets securely and wipe your input buffers; the
library copies them into guarded storage. `node->identity()` gives the public
keys once available. Independently exchange and verify the other peer's
signing public key before authorizing it. Discovery proves the advertisement's
identity, not that your application should trust it.

## Start, discover, and connect

1. Call `node->start(completion)` on the executor. A `true` return admits
   startup; inspect the later `NodeStartResult`. `Started` means the local
   backend started, not that bootstrap succeeded or peers are reachable.
2. Call `node->watch(capability, scheduler, options, handlers)` and retain the
   returned `Subscription`. Its handlers receive verified advertisements,
   catch-up completion, errors, and optionally recovery status. A result is
   either a handle or `WatchError`; check it before continuing.
3. Call `node->announce(capability, scheduler, options, handlers)` and retain
   the returned `Lease`. The publication callback confirms backend acceptance,
   not that another peer saw the record. `AnnouncementError::Retrying` is
   nonterminal.
4. Filter advertisements by your independently verified peer key. Check
   `subscription->is_current(advertisement)` before using a stored record;
   callbacks can arrive after a record becomes stale. Watch catch-up and `Ready`
   do not prove the DHT contains every peer.
5. The initiator calls `node->connect(*lease, *subscription, peer, ice,
   scheduler, options, handlers)`. The responder calls `node->listen(...)`,
   validates the peer in `on_request`, then calls `node->accept(*listener, ice,
   scheduler, options, handlers)`. Retain successful handles and inspect
   immediate error variants. Closing a listener rejects a pending request, with
   best-effort notification to the peer. The listener is single-shot.
6. Wait for the connection's `on_state(Ready)` callback before sending.
   Initial `Connecting` is not delivered as an event. `Suspended` means the
   channel is temporarily unavailable; `Failed` and `Closed` end its use.

For multiple incoming requests, set `IncomingOptions::persistent = true` and
provide `IncomingHandlers::on_request_identified`. Each callback receives a
listener-scoped request ID and its verified advertisement. Authorize the peer,
then either call `node->accept(*listener, id, ...)` or `listener->reject(id)`.
The listener stays `Listening` and other requests remain pending. An invalid,
expired, rejected, or already accepted ID cannot be accepted again. The
single-shot `peer()`, `request_id()`, and ID-less `accept` methods apply to the
default mode.

`max_pending_requests` bounds persistent requests (1–128, default 16); their
payloads also share a 1 MiB budget. A full queue rejects a new offer best effort
without consuming existing requests; optional `on_overflow` notifications are
coalesced. Optional `on_request_ended` reports timeout or authenticated remote
cancellation for an ID. Closing the listener rejects all pending requests best
effort and suppresses callbacks not already executing. Callbacks are deferred on
the owner executor, and IDs are local to the listener.

The pending limit is an upper bound, not a reservation. A separate replay cache
holds at most 32 recent offers per lease, including offers already accepted or
rejected. If it fills before entries expire, new offers are dropped (their
peers may time out) and the listener stays open; entries are never evicted
early, so replay protection is unchanged and capacity returns as entries
expire. Reopening the listener does not clear this cache. Offers from peers
whose advertisements have not been seen yet are held in a table of 16 that
evicts the oldest entry when full. Local failures such as allocation can still
fail the listener. Applications should handle listener failure and retry when
appropriate.

The echo example waits for both publication and watch catch-up before starting
its connection, and retains a matching advertisement until it can dial. Its
`watch`, `announce`, `try_connect`, `listen`, `connect`, and `request` methods
show the exact handler signatures and result checks.

Watches retry transient store failures with bounded backoff. Inspect
`Subscription::recovering()` and `healthy()`; `DiscoveryStatus::Ready` means
the watch recovered and catch-up completed, not that discovery is complete.
Polling-based browser watches can miss short-lived records. An established
connection can continue while discovery is interrupted.

## Send and receive

Supply all required `ConnectionHandlers`: `on_state`, `on_route`, `on_message`,
`on_writable`, and `on_error`. `on_status` is optional. Received `Bytes` are
owned by the callback. `Connection` methods run on the owner executor.

`connection->send(ByteView)` returns `SendStatus`:

| Status | What to do |
| --- | --- |
| `Accepted` | The message was queued locally; this is not a delivery or persistence acknowledgment. |
| `Backpressure` | Retain the message in your application and retry after `on_writable`. |
| `NotReady` | Wait for a ready state, or handle a suspended connection. |
| `TooLarge` | Split the application data or use a different transfer protocol. |
| `Failed`, `Closed` | Stop sending and handle the connection failure or closure. |

The negotiated `max_message_size()` is available after authentication and
while suspended. The default local limit is 256 KiB; `ConnectOptions` permits
up to 4 MiB, subject to its pending-byte limit and the peer's advertised
limit. The library fragments individual messages internally; it does not offer
file streaming, durable delivery, acknowledgments, or CRDT synchronization.
The application must implement those where needed.

The example fails on backpressure for simplicity. A real application should
manage a bounded queue. `Connection::close()` suppresses callbacks not yet
delivered; a callback already executing may finish. Poll `state()` when
exceptional scheduler failures may have prevented a callback.

## Datagrams

For data where only the latest value matters, such as pointer positions, a
connection can also send datagrams. They do not wait behind reliable messages
and are never retransmitted, so they may be lost or arrive out of order.

```cpp
handlers.on_datagram = [](Bytes bytes) { /* newest data wins */ };
if (connection->datagrams_available())
    (void)connection->send_datagram(bytes);
```

A datagram holds at most `max_datagram_bytes` (1024) bytes, so that it stays
within one network packet. They use a second data channel in the same DTLS
session and are delivered only after the peer is authenticated. `on_datagram`
is optional; without it, received datagrams are dropped.

`send_datagram` returns `Accepted` when the datagram was handed to the network,
`NotReady` while datagrams are unavailable, `TooLarge` above the limit, and
`Backpressure` when too much is waiting; drop it or send newer data later, as
`on_writable` does not report datagram capacity. Neither a failed datagram nor
a full receive queue affects the connection.

`datagrams_available()` becomes true shortly after `Ready`, once the channel
opens. It stays false when either side sets `ConnectOptions::datagrams` to
false or runs a version without datagram support; such peers still connect
normally, so keep a reliable fallback for anything that must arrive.

## STUN, TURN, and route reporting

Pass an `IceConfig` to `connect` or `accept`. `servers` accepts `StunServer`
and `TurnServer` values; hosts are DNS names or bare IP addresses, not URLs.
TURN supports UDP, TCP, and TLS. Set the TLS port explicitly when needed.
Credentials are ordinary strings supplied for each attempt; the application
protects and refreshes them. Live credential refresh is not provided.

Automatic policy can use a direct route or TURN. `RequireLocalRelay` requires
the local endpoint to use a relay, but has not been verified as a guarantee
that all direct ICE traffic is prevented. Native TURN/TLS requires the patched
libnice backend. The native backend currently selects one configured STUN
server. See [TURN testing](TURN_TLS_TESTING.md) for the tested conditions.

`on_route(RouteInfo)` reports `Direct`, `TurnRelayed`, or `Unknown`, plus
selected candidate information when available and observed TURN history.
`Unknown` is not evidence that TURN was unused: peers can have different
visibility of the selected route, particularly with peer-reflexive candidates.
The route can change or become unknown during a connection.

`RouteInfo::round_trip_time`, also returned by `Connection::round_trip_time()`,
is the smoothed round-trip time of the selected path, as WebRTC measures it:
the SCTP association's estimate natively (libdatachannel `rtt()`), and the
selected candidate pair's `currentRoundTripTime` from `getStats()` in browsers.
It is sampled about every two seconds and smoothed with a gain of 1/4, so it
follows changes within ten seconds or so. It is empty until the first sample
and whenever no path is selected (suspended, failed or closed), and it can stay
empty where a browser does not report the statistic. It needs no extra traffic.
`on_route` fires when the route changes, the round-trip time included (so at
most every two seconds for it), not on every observation; `route()` always
returns the latest observation.
Treat it as a hint for choosing peers, not as a precise measurement.

## Limits, errors, and security boundaries

Error enums distinguish admission, signaling, authentication/protocol,
transport, timeout, cancellation, and resource-limit failures where the
backend supplies enough information. Some failures remain generic. A
`ConnectionStatus::SignalingUnavailable` callback means the authenticated
channel remains usable but the attempt can no longer exchange new signaling.

The default connection timeout is 30 seconds. A node retains up to eight
connection handles by default; set `DiscoveryConfig::max_connections` to a
value from 1 through 1024 to change the per-node limit, shared across all its
capabilities and including connecting and retained closed or failed handles.
A released handle can continue occupying its slot until queued backend cleanup
finishes; an attempt made during that interval may return `ResourceLimit`.
A node can retain up to 16 discovery subscriptions; announcement and
signaling have additional bounds. Signaling descriptions and candidates are
bounded and are not chunked in this alpha. Limits do not cover all backend or
process memory. See [discovery and signaling](DISCOVERY_SIGNALING.md) and
[`node.h`](../include/p2pdc/node.h) for exact limits.

The capability is secret bearer material; public keys require independent
verification and application authorization. Browser page code can access
WebAssembly memory. Discovery and proxy services can observe network activity;
TURN relays carry encrypted traffic and can observe connection metadata. The
protocol and security boundaries remain under review; this alpha is not
intended for production use.

## API observations

Writing this guide exposes some possible improvements to consider, rather than
changes promised by the current API:

- A higher-level session helper could manage watch/lease readiness, peer
  authorization, single-shot listeners, and reconnection without duplicating
  the state machine in every application.
- Shutdown and executor ownership are precise but demanding. A simpler
  lifecycle abstraction and clearer exceptional-submission reporting could
  reduce accidental misuse.
- Typed errors are useful, but diagnostics and timings for discovery,
  signaling, ICE, and authentication would make slow or failed connections
  easier to understand.
- A built-in bounded send queue, optional disposable messages and a credential
  provider would cover common application needs.
- A curated top-level header and installed package could make the intended
  application interface easier to distinguish from lower-level protocol tools.
