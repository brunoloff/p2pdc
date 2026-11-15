# WebAssembly implementation notes

These development notes cover internal adapters and earlier tests. The current
public C++ interface and release status are described in the project README.
Run the commands below from the project root.

To repeat the internal C++ record-store interoperability test on Linux, build
`p2pdc_browser_record_native_peer` in `build-native` and
`p2pdc_browser_record_store_smoke` in `build-wasm-auth`, then run
`bash tests/run_browser_record_store_smoke.sh`. It requires Chromium, OpenDHT's
`dhtnode`, `unshare`, `ip`, `curl`, and Python's `websocket` module. It uses an
isolated network namespace and HTTP loopback, not a public HTTPS proxy.

The experimental browser-side `src/browser/opendht_proxy.mjs` implements the
OpenDHT proxy's get, put, and streaming listen routes. It needs an explicitly
configured HTTPS/CORS proxy (HTTP is accepted only for local loopback testing).
Records are untrusted until verified by the security layer. Lookup limits apply
to retained unique records, not total duplicate traffic; listen applies its
byte limit per record. `ProxyMailbox` starts catch-up after the listen response
opens; `ProxyOutbox` serializes bounded publications and retains failed records
for explicit retry. Response headers do not prove DHT watch registration, and
catch-up is not a delivery barrier. The owner must verify and deduplicate
records. Raw `ProxyMailbox` stream interruption is terminal; browser discovery
uses the recovering wrapper described below. Run
`node --test tests/browser_proxy_test.mjs` for isolated
stream tests. The opt-in `tests/browser_proxy_live_test.mjs` requires
`P2PDC_TEST_PROXY_URL` pointing to a controlled proxy.

The experimental `WasmDiscovery` bridge derives the group lookup key and
verifies advertisements using the C++ security code. It rejects stale or
conflicting sequences and expires peers, but its replay state is in memory and
resets on restart. Updated peers receive new handles; consumers replace entries
by signing key and instance, authorize the peer independently, and recheck
`current()` immediately before connecting. A verified advertisement does not
prove reachability or grant consent.

Experimental `BrowserWatch` and `BrowserLease` handles add timed expiry,
announcement refresh, and bounded retry. A lease exposes its endpoint before
publication so its owner can open the signaling inbox first; it reports a
published endpoint only after proxy acceptance. Announcements use a 120-second
lifetime and refresh after 60 seconds, with at most 32 publication attempts per
rotating window (or per fixed-key lease), including retries. A failed or expired pending publication terminates the
lease. Browser timer suspension can interrupt availability; opening a stream
or completing catch-up does not prove network-wide visibility.

After a successful browser discovery start, temporary proxy stream or network
failures trigger up to five retries by default, with exponential backoff from
one second. Each attempt opens a new stream and performs a fresh lookup. The
optional `onStatus` callback reports `reconnecting` and then `ready`; new node
connections and incoming consent are unavailable during recovery. A pending
incoming request can be accepted again after recovery if it has not expired;
`accept()` returns false while recovery is in progress. Signaling received in
that period is bounded and rechecked before the accepted connection sees it. Retry limits
and initial delay can be set through `watch.recovery` in `BrowserNode` options.
Malformed records, resource-limit errors, non-retryable HTTP errors, exhausted
retries, and an initial startup failure remain terminal. Initial startup can be
tried again with a new node. Existing authenticated data channels remain open;
in-progress attempts may fail. Reconnection and catch-up are best effort and
cannot recover records that expired while disconnected. Signaling mailboxes
still treat interruption as terminal. The opt-in
`tests/browser_recovery_live_test.mjs` uses `P2PDC_TEST_PROXY_URL` and a
controlled proxy; it injects a local stream interruption and checks catch-up.

The separate experimental `src/browser/webrtc_transport.mjs` provides a raw
ordered binary Data Channel and sampled route reporting. It does not itself verify
signaling or peer identity; the C++ node adds those checks. Its
callbacks must be synchronous; callers handle asynchronous signaling failures.
Local candidates are bounded and delivered after the local description, so a
signaling layer can sign the description first. Local in-app-browser tests have
exercised direct and TURN-relayed exchanges.

The experimental `src/browser/wasm_reliable_link.mjs` bridges that transport
to the C++ reliable framing code compiled to WebAssembly. It handles bounded
binary messages and deferred callbacks, but provides no authentication or
discovery and is not a public browser `Node` implementation. The local browser
smoke tests exercise fragmented messages between browser peers and between a
native offerer and a browser answerer. These particular framing tests are
unauthenticated; other browser/platform combinations remain unverified.

The independent core components can be compiled with Emscripten. A compatible
WebAssembly build of libsodium is needed for the security components;
production-ready browser integration remains pending.
The internal `AsyncSignaling` component withholds the handshake context until
the caller reports successful installation of both signed descriptions.
The internal `SecureChannel` combines
that context, the handshake gate and reliable framing; applications cannot send
or receive through it until the handshake completes. Its owner must ensure the
signaling and WebRTC transport belong to the same attempt, enforce fingerprint
verification, and close the underlying transport on failure.
The internal `SecureAttempt` owns both components for one preauthorized attempt
and handles terminal signaling controls and the handshake deadline. The
experimental `src/browser/wasm_secure_link.mjs` exposes that attempt to browser
adapters. Its owner must supply preauthorized peer keys and exact installed SDP,
copy and protect credential inputs, close the actual WebRTC transport on
failure, and reject candidates without a usable MID. WebAssembly does not
isolate secrets from other JavaScript in the same page. A same-page browser
smoke test has exchanged authenticated messages over WebRTC using direct
signaling and fixed test identities. This test now uses the internal
`BrowserConnection` owner, which combines the secure link and WebRTC transport;
its caller must still provide authorized peer bindings. Signaling can arrive out
of order within the bounded replay window; candidates wait for SDP installation.
It is not a public discovery or incoming-consent API. A separate local browser test discovered
and verified advertisements, authorized the expected test identities, and
carried authenticated signaling through a proxy and two isolated OpenDHT
nodes before exchanging messages. That test is
`tests/browser_proxy_secure_smoke.html?proxyPort=PORT`, served locally with a
controlled loopback proxy; it does not start the proxy itself. Adding
`&turn=both` uses a test TURN server on loopback port 34789 and requires both
peers to report a relayed selected pair. This passed locally with temporary
test credentials. `&turn=one` requires only the answerer to use TURN; that
case timed out in local testing despite both candidates being applied: ICE did
not establish a candidate pair on the answerer. The timeout was also reproduced
with direct browser WebRTC signaling (`tests/browser_webrtc_smoke.html?turn=one`)
and with a raw `RTCPeerConnection` test
(`tests/browser_raw_ice_smoke.html?turn=one`), without this library's signaling
or transport wrapper. The raw test passed when both peers used the relay
(`?turn=both`); the transport wrapper passed direct and two-relay controls.
Those tests used one browser instance and a local coturn server; they do not
identify the cause or establish internet behavior. The cause remains unresolved.
In the same local raw test, adding `&end=1` sent and applied both browser
end-of-candidates markers, but the one-sided case still timed out. A two-relay
control completed after applying both markers. This does not replace protocol
support for completion markers: DHT records may arrive out of sequence, so an
end marker must not be applied ahead of earlier candidates.
With the TURN server configured on the automatic offerer as well, the same
local authenticated discovery/signaling test passed using `&turn=auto`: the
answerer required a local relay and both peers reported a relayed route. In the
direct transport test (`tests/browser_webrtc_smoke.html?turn=auto`), one run
selected a host-to-relay pair on the offerer and relay-to-peer-reflexive on the
answerer. A separate isolated Chromium test now covers two automatic peers with
direct traffic blocked, including unavailable and invalid TURN credentials.
A local native-offerer/browser-answerer
test also verified authenticated fragmented messages in both directions and
rejected an altered signaling record. A production-ready browser node, automatic TURN
fallback through this bridge, cross-network interoperability, and other
platforms remain unverified or unfinished.

The internal `WasmIncoming` bridge can match an encrypted incoming offer to a
current, verified browser discovery record using the native offer resolver. A
local browser smoke test covers unknown-peer retry, altered records, replay
across wrapper recreation, mismatched credentials and stale handles. Admission
identifies a peer; it does not grant consent. Its owner must recheck the lease
and peer when accepting, and provide bounded handling for records received
before discovery catches up. The internal `BrowserListener` now owns an inbox
and announcement, retains bounded early records, and requires explicit consent
before handing an offer to a connection. Invalid early records are discarded,
while authenticated non-offer signaling
is retained within the existing bounds for the accepted connection. A
relay-required browser transport also buffers inbound data within its limits
while rechecking temporarily unavailable route information.
The local browser test covers that handoff with an in-memory backend and with a
local OpenDHT proxy and two
isolated DHT nodes. The latter also passed with both peers using a controlled
local TURN server and reporting the selected relay. These tests do not establish
internet reachability or reliable discovery; native/browser API parity and
cross-network listener integration remain pending.

The experimental `BrowserOutgoing` owner combines a single outgoing attempt's
lease, inbox, signaling outbox and authenticated connection. The caller supplies
an active watch and authorizes a verified peer; the owner rechecks that peer
before connecting. Its announcement closes after authentication, while the
connection retains its signaling resources. Local tests exercised this owner
through the OpenDHT proxy and a TURN relay. It is a building block, not yet a
complete browser node on its own. After authentication, a temporary signaling
backend failure reports `signalingUnavailable` through `onStatus` and leaves an
otherwise healthy data channel open. Signaling is not restored for that attempt,
so new ICE candidates cannot be exchanged; a later transport failure still ends
the connection. Before authentication, or for invalid data and resource errors,
signaling failure remains terminal. The native connection likewise retains an
authenticated channel on a detected backend outage, reporting it through the
optional `ConnectionHandlers::on_status` callback. OpenDHT does not necessarily
report an outage while no signaling operation is underway, so absence of this
status does not establish that discovery or signaling is healthy.
Local native tests inject a backend failure into an authenticated connection
and verify status delivery, continued messaging, and callback suppression on
close; they do not simulate an internet or OpenDHT network outage.

The matching experimental `BrowserIncoming` owner keeps consent explicit and
combines the listener, signaling outbox and authenticated connection. Its
`reject()` closes the request locally; it does not yet send a rejection record.
The caller still owns the discovery watch and must call `retry()` when discovery
changes. Local testing exercised both owners together through an isolated
OpenDHT proxy and TURN server.

The experimental `BrowserNode` composes these owners for one discovery
capability: `start()` opens discovery, `connect()` starts an outgoing attempt
for a peer object emitted by that node, and `listen()` starts a single incoming
attempt requiring explicit application acceptance. It rechecks peer freshness,
retries pending incoming offers when discovery changes, and bounds concurrent
handles to eight. `close()` closes owned handles and suppresses queued node
callbacks. Discovery failure closes pending attempts but leaves authenticated
connections available until closed. The node keeps a JavaScript copy of its
credentials until close or failure; other code in the page can access them.
The local `tests/browser_node_smoke.html?proxyPort=PORT&turn=both` test uses an
isolated proxy and test TURN server. The interface is experimental, single-group
and not yet equivalent to the intended complete C++ API.

A separate local test has connected the public native `Node` and browser in
both offerer/answerer arrangements through the same isolated OpenDHT network,
then exchanged authenticated messages in both directions. The test uses fixed
test credentials and a local proxy; it is not an internet or mobile-platform
test. Its native helper is `p2pdc_browser_native_dht_peer`, and its browser page
is `tests/browser_native_dht_smoke.html`. Run it serially against a fresh,
isolated OpenDHT network ID 873198, bootstrapped at `127.0.0.1:49411` with its
proxy on `127.0.0.1:49412`. The existing `browser_interop_server.py --secure`
can launch the native helper and serve this page; the page checks the helper's
successful exit using the server's completion handshake. The fixed test
capability must not be used for real sessions. Set `P2PDC_TEST_NATIVE_ROLE`
to `answerer` and use `tests/browser_native_offer_smoke.html` to test the reverse
roles. The native listener now retains a bounded set of authenticated offers
while waiting for their peers' advertisements; retries and discovery remain
best effort.

The public experimental `BrowserNode` has also passed this local exchange with
the native `Node` in both directions. Use the same helper and server with
`tests/browser_native_node_smoke.html?role=incoming` for a native offerer, or
set `P2PDC_TEST_NATIVE_ROLE=answerer` and use `?role=outgoing` for a browser
offerer. Restart the isolated DHT network between runs to clear test records.
Both peers use fixed test identities; this does not test TURN or internet paths.
For the asymmetric message-limit test, set
`P2PDC_TEST_SMALL_MESSAGE_LIMIT=1` on the native helper's server and add
`&small=1` to either browser page mode. Both peers then verify the negotiated
8 KiB limit while exchanging authenticated messages. To test a lower browser
limit instead, set `P2PDC_TEST_BROWSER_SMALL=1` on the server and add
`&browserSmall=1` to the page URL.
For the 4 MiB bidirectional fragmentation and backpressure test, set
`P2PDC_TEST_LARGE_MESSAGE=1` on the server and add `&large=1` to the public
browser-node page URL.
To build the core and the experimental framing bridge:

On the Manjaro package, add `/usr/lib/emscripten` to `PATH` first if `emcmake`
is not already available.

```sh
emcmake cmake -S . -B build-wasm -G Ninja -DBUILD_TESTING=OFF \
  -DP2PDC_ENABLE_SODIUM=OFF -DP2PDC_BUILD_BROWSER_RELIABLE=ON
cmake --build build-wasm --target p2pdc_browser_reliable --parallel 1
```

The browser handshake smoke test additionally needs a static libsodium build
for WebAssembly. It exercises the C++ handshake with matching and mismatched
synthetic contexts and a secure framed exchange using signed synthetic
signaling. With a compatible Emscripten build of libsodium,
configure `P2PDC_ENABLE_SODIUM=ON`, `P2PDC_BUILD_BROWSER_SECURITY_TEST=ON`,
`SODIUM_INCLUDE_DIR` and `SODIUM_LIBRARY` for that build, then build the
`p2pdc_browser_handshake_smoke` target. The test page expects that target in
`build-wasm-auth` and must be served over localhost or HTTPS.
Set `P2PDC_BUILD_BROWSER_SECURE=ON` and build `p2pdc_browser_secure` to run the
experimental authenticated bridge smoke tests.

The local test build used the upstream [libsodium 1.0.22 release](https://github.com/jedisct1/libsodium/releases/tag/1.0.22-RELEASE)
with SHA-256 `adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349`.
It was configured with Emscripten for `wasm32-unknown-emscripten`, static-only,
without assembly or pthreads, and built with one job. The dependency under
`.deps/` is a local build artifact and is not checked in.

To reproduce that local dependency from the project directory:

```sh
mkdir -p .deps
curl -fL -o .deps/libsodium-1.0.22.tar.gz \
  https://download.libsodium.org/libsodium/releases/libsodium-1.0.22.tar.gz
echo 'adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349  .deps/libsodium-1.0.22.tar.gz' | sha256sum -c - &&
  tar -xzf .deps/libsodium-1.0.22.tar.gz -C .deps
export EM_CACHE="$PWD/.deps/emscripten-cache" EMCC_CORES=1
cd .deps/libsodium-1.0.22
emconfigure ./configure --host=wasm32-unknown-emscripten \
  --prefix="$PWD/../sodium-wasm" --disable-shared --enable-static \
  --disable-asm --disable-pie --without-pthreads CFLAGS=-O1
emmake make -j 1
emmake make -j 1 install
cd ../..
emcmake cmake -S . -B build-wasm-auth -G Ninja -DBUILD_TESTING=OFF \
  -DP2PDC_ENABLE_SODIUM=ON -DP2PDC_BUILD_BROWSER_SECURITY_TEST=ON \
  -DSODIUM_INCLUDE_DIR="$PWD/.deps/sodium-wasm/include" \
  -DSODIUM_LIBRARY="$PWD/.deps/sodium-wasm/lib/libsodium.a"
cmake --build build-wasm-auth --target p2pdc_browser_handshake_smoke --parallel 1
```

On Linux, with the native integration-test build configured, the local
native/browser framing smoke test can be started with:

```sh
cmake --build build-native --target p2pdc_browser_interop_peer --parallel 1
python tests/browser_interop_server.py ./build-native/p2pdc_browser_interop_peer
```

Open the printed loopback URL in a browser, then stop the server after the result.
This harness is for local testing only; its signaling is not authenticated.

For the authenticated native/browser test, build the security-enabled native
and WebAssembly targets, then run:

```sh
cmake --build build-native --target p2pdc_browser_secure_interop_peer --parallel 1
python tests/browser_interop_server.py --secure ./build-native/p2pdc_browser_secure_interop_peer
```

This forwards signed encrypted test signaling through the same local harness.
It uses fixed test keys and does not exercise OpenDHT discovery or TURN.
