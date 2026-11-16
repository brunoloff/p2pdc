# p2p-discovery-connect

This library is licensed under the GNU General Public License, version 3 or
any later version; see [COPYING](COPYING). Third-party dependencies and their
upstream code retain their respective licenses.

An experimental C++ library providing OpenDHT discovery and
authenticated WebRTC data channels with explicit TURN route reporting. Its
payloads are application-neutral; it does not depend on a CRDT implementation.

The experimental public C++ API provides asynchronous discovery, peer
authorization, and binary connections on native and WebAssembly targets using
the same consumer code. Linux and browser peers have exchanged messages locally;
Android has passed on-device public-node tests and direct and relay-required
Linux-to-phone internet echoes, including automatic Android TURN fallback. The
first alpha targets Linux and the tested Chromium and Firefox versions; Android
work resumes tomorrow.
Resource limits do not bound all process or backend memory. Do
not use the library for production.

See [first alpha status](docs/ALPHA_STATUS.md) for the current validation and
remaining release work. The original plan and Xournal++ survey are retained as
[historical references](docs/history/README.md).

For application developers, start with the [public C++ API guide](docs/PUBLIC_API.md).

`p2pdc/ice_config.h` defines backend-independent STUN and TURN server settings
and automatic or local-relay-required policy. Native connection attempts use
these settings, including the experimental outgoing `Node::connect` interface.
Hosts are DNS names or bare IP addresses, not URLs. Credentials are copied
as ordinary strings. For alpha, the application obtains and refreshes credentials
and supplies them for each new connection attempt; the library does not refresh
credentials on a live attempt, which can limit later ICE recovery. The native
backend selects one configured STUN server; TURN
configurations require the patched libnice backend. Native TURN-over-TLS uses
the patched backend and OpenSSL's default trust store; it is rejected if that
backend is not loaded. Controlled Linux, Chromium and Firefox tests checked trusted and
untrusted certificates and hostname mismatch. Other browser versions and
real-network relay-only enforcement remain unverified.

`p2pdc/discovery_config.h` defines native OpenDHT bind, network and bootstrap
settings and browser proxy URLs in one `DiscoveryConfig`. Native builds ignore
the proxy URLs; the WebAssembly backend requires one browser-accessible proxy.
The preliminary `p2pdc/node.h` interface accepts these settings and
supports asynchronous native startup and shutdown when OpenDHT is enabled.
With libsodium enabled, `Node::watch` provides bounded subscriptions to verified
peer advertisements, and `Node::announce` provides finite signed-presence leases.
Both use an application-provided timer scheduler. Startup and catch-up do not
establish bootstrap health. Watches retry transient store interruptions with a
bounded backoff and repeat catch-up. `Subscription::recovering()` and the
optional `DiscoveryHandlers::on_status` distinguish reconnection from a terminal
error; `healthy()` is false during recovery. `Ready` means the watch and its
catch-up completed, not that the DHT contains every peer. `WatchOptions` controls
the retry count and initial delay. An already established data channel is not
closed merely because discovery is interrupted. By default, announcements use an ephemeral node
identity and a fresh instance per lease. Applications that need a stable
identity can call `Node::create_with_identity` with an Ed25519 signing seed and
an X25519 recipient secret; `Node::identity` returns their public keys. The
caller owns and must securely store and wipe both input buffers. Imported
secrets use libsodium-allocated storage; WebAssembly memory remains accessible
to other code in the page. This factory requires the discovery backend and
libsodium; without that support it returns `Unsupported`.
Existing records can remain until expiry after a lease
closes. Announcements do not establish that an endpoint can be contacted.

### Default runtime

`p2pdc::Runtime` supplies a serial executor and timer scheduler. Native builds
use one worker thread; WebAssembly uses the browser event loop. Pass
`runtime.executor()` to `Node::create`, and pass
`runtime.scheduler()` to operations that require a scheduler. Post calls to
node methods to that same executor; their callbacks run on that executor.
For GUI updates, dispatch from those callbacks to the GUI's own event loop.
Applications may instead supply their own executor and scheduler as before.

Keep the runtime alive until every node has completed asynchronous shutdown and
its handles have been released. Destruction discards queued work and waits for
the current task when called from another thread. If called from the worker,
it returns immediately and that worker exits after its current task. Tasks must not
throw or block the worker. Posting can throw on allocation failure or after
shutdown; callers must observe the node's existing exceptional submission
handling. Canceled timer captures are released asynchronously by the worker.
The runtime's task and timer queues are not independently memory bounded.
On WebAssembly, call the runtime from the browser event-loop thread and do not
block it. The C++ `Node` supports discovery, incoming offers, and connections
on both native and WebAssembly targets. A selected peer-reflexive candidate
without a known relay is reported as `Unknown`: the other peer may be relayed
without that fact being visible in local ICE statistics. The library does not
yet exchange peer route observations, so the two endpoints can report different
levels of certainty.
If a browser route observation stalls for two seconds, the route becomes
`Unknown`; a relay-required connection suspends until fresh evidence is
available. The library waits for the outstanding browser request to settle
before starting another, so repeated polls do not accumulate requests.
Background execution remains subject to platform suspension and does not
guarantee continuous availability.

The internal C++ mailbox and publisher use a backend-neutral record-store
interface, as do the discovery cycle, public watch and lease factories, and
rotating-window mailbox.
It has native OpenDHT and browser HTTPS-proxy adapters; the browser adapter
bundles its JavaScript bridge with the WebAssembly target. A controlled
local test exchanges records in both directions between a native OpenDHT node
and the browser proxy, and exercises publication and discovery through the C++
factories using that proxy. The experimental browser C++ `Node` uses this adapter
for discovery and exposes the same `connect` and `accept` methods as native.
A local two-browser test using default rotating discovery exchanged authenticated
messages through the public C++ API. The same C++ consumer also passes natively.
Separate native/WebAssembly public-node tests have exchanged messages in both
offerer/answerer arrangements over an isolated local OpenDHT network. Build
`p2pdc_browser_native_dht_peer` and `p2pdc_browser_public_connection_smoke`, then
run `bash tests/run_browser_native_public_smoke.sh native-offerer` and repeat with
`browser-offerer`. The isolated proxy interruption and recovery check is
`bash tests/run_browser_native_recovery_smoke.sh`; it also exchanges data while
the proxy is unavailable.
To leave capacity for publications and lookups with HTTP/1.1, the browser shares
identical watches, limits long-lived streams to two per proxy origin, and polls
additional keys every 750 ms. If stream startup takes two seconds, its watch
also switches to polling. Polled snapshots are bounded and deduplicated;
their absence does not prove that a value expired, and short-lived values can be
missed between polls. The aggregate fingerprint content in the polling cache is
limited to 16 MiB per WebAssembly module; container overhead is not included.
Browser connections remain experimental.
Each browser store reserves its configured maximum response size for each
pending lookup, with a 16 MiB total reservation limit; excess concurrent
lookups report `ResourceLimit`. This limit does not cover all browser memory.
DHT records are unauthenticated: on native and browser stores, records larger
than the configured value size, or beyond a lookup's or watch batch's record
and byte limits, are dropped and counted rather than ending the lookup, watch or
mailbox. A flood of records can still crowd out legitimate ones until they
expire, but it cannot permanently stop discovery or signaling.

## Native build and tests

Native example launchers are collected in `bin/`: `p2pdc_shared_text`,
`p2pdc_echo`, and `native_smoke`. The first two prefer `build-native-tls` and
fall back to `build-native`; the smoke launcher uses `build-native-smoke`. They
report a build command if their executable is missing.
`bin/p2pdc_shared_text_browser` serves the WebAssembly editor on loopback and
opens it in the default browser; press Ctrl+C in that terminal to stop it.

The [shared text editor example](examples/shared-text/README.md) is built by
default and additionally needs Qt 6 and Yrs. Use
`-DP2PDC_BUILD_SHARED_TEXT=OFF` to build the library without those dependencies.

The default native build includes the discovery and WebRTC backends. It needs a
C++20 compiler, CMake, libsodium, OpenDHT, msgpack-cxx, and libdatachannel;
the default OpenDHT lookup also uses pkg-config. Set `CMAKE_PREFIX_PATH` if a dependency is installed outside the
usual search paths. GoogleTest is needed when `BUILD_TESTING` is enabled. The
integration tests start isolated loopback nodes and do not use a public bootstrap
server. For low-level development, `P2PDC_ENABLE_OPENDHT=OFF` or
`P2PDC_ENABLE_NATIVE_TRANSPORT=OFF` disables the corresponding backend and the
public node or connection functionality that depends on it. WebAssembly builds
select browser adapters by default. A parent project or cross-compilation toolchain
may provide an existing OpenDHT CMake target and set `P2PDC_OPENDHT_TARGET` to
its name; otherwise the native build uses `pkg-config`. The supplied target must
carry the correct target-architecture headers and link dependencies. Provide a
`msgpack-cxx` target as well, or make its CMake package available. The build
also reuses an existing `LibDataChannel::LibDataChannel` target; it must provide
the required patched libdatachannel described below.

[Dependency versions used for the alpha checks](docs/DEPENDENCIES.md) include
the exact patched upstream commits and the host-provided library versions.

The reliable channel's wire layout and shared test vectors are documented in
[version-1 framing](docs/FRAMING_V1.md). The provisional discovery and signaling
record formats are described in [discovery and signaling](docs/DISCOVERY_SIGNALING.md).
When Python is available, CTest also checks the independent framing and discovery
vectors; PyNaCl enables the independent encrypted-record and signature checks.

```sh
cmake -S . -B build -G Ninja -DP2PDC_TEST_OPENDHT=ON
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
```

`bash tests/run_frame_fuzz.sh` runs a bounded libFuzzer check of the frame
decoders with AddressSanitizer and UndefinedBehaviorSanitizer. It needs Clang
with those runtimes; a short run is not an exhaustive security audit.
`bash tests/run_record_fuzz.sh` similarly exercises advertisement, signaling,
candidate, and handshake decoders using seeded records; it also needs PyNaCl.
Set `P2PDC_FUZZ_CXX` to select a compatible Clang. On this host Clang 23
reported an allocation mismatch even for a minimal standalone libFuzzer
program; Clang 21 ran the checks without disabling sanitizers. Failed runs
retain their temporary corpus and artifacts for inspection.

Set `P2PDC_TEST_OPENDHT=OFF` to omit the OpenDHT tests. The OpenDHT store is
built only when `P2PDC_ENABLE_OPENDHT=ON`. The security primitives
are enabled by default; `P2PDC_ENABLE_SODIUM=OFF` builds only the other core
components and is not a complete or secure discovery implementation.

The [shared C++ echo peer](examples/echo/README.md) demonstrates the public
`Node` API with explicit peer authorization on native and WebAssembly targets.
The optional [Yrs reference workload](integrations/yrs/README.md) exercises
offline edits and catch-up between two native peers without adding Yrs to the
library's dependencies.

To consume the source from another CMake project, configure its dependencies
for the target platform, then add the library and link its public target:

```cmake
add_subdirectory(path/to/p2p-discovery-connect)
target_link_libraries(your_application PRIVATE p2pdc::p2pdc)
```

The target propagates its public C++ requirements and, for Emscripten, the
browser bridge link options. The browser application still needs an event-loop
host and a reachable OpenDHT proxy. The project does not yet provide an
installed CMake package; this integration uses the source tree. The small
`tests/external_consumer` fixture checks an independent project's public-target
integration on native and WebAssembly; the echo tests exercise actual messaging.

The separate [native smoke example](examples/native-smoke/README.md) tests
libdatachannel direct and TURN data channels using in-process signaling. It does
not exercise the library's discovery or connection API.

The experimental native transport is enabled by default; add
`-DP2PDC_TEST_NATIVE_TRANSPORT=ON` to run its tests. It requires the pinned libdatachannel v0.23.2 patch in
`patches/libdatachannel-v0.23.2-p2pdc.patch`. An unpatched library lacks the
required feature symbol and cannot be linked. Relay-only mode additionally
requires the libnice backend and its `force-relay` property. This setting and
selected-pair checks have not yet been independently verified to prevent all
direct ICE traffic; do not rely on this mode for network privacy yet.

For a fresh Linux checkout, the following builds the patched dependencies
locally. It requires GLib/GIO development files, OpenSSL, Meson, pkg-config,
Git, Ninja, and a C/C++ toolchain:

```sh
git clone https://github.com/libnice/libnice.git .deps/libnice-tls-src
git -C .deps/libnice-tls-src checkout af62014d520ac09c8fe1974bda1b253114c68843
git -C .deps/libnice-tls-src apply ../../patches/libnice-v0.1.24-p2pdc-tls.patch
meson setup .deps/libnice-tls-build .deps/libnice-tls-src \
  --prefix "$PWD/.deps/libnice-tls-install" --libdir lib \
  -Dcrypto-library=openssl -Dgupnp=disabled -Dgstreamer=disabled \
  -Dintrospection=disabled -Dtests=disabled -Dexamples=disabled
CCACHE_DISABLE=1 meson compile -C .deps/libnice-tls-build -j 1
meson install -C .deps/libnice-tls-build --no-rebuild
git clone https://github.com/paullouisageneau/libdatachannel.git .deps/libdatachannel
git -C .deps/libdatachannel checkout 9e6a13abbb6846c003d817d0387b6706466e2b03
git -C .deps/libdatachannel submodule update --init --recursive --jobs 1
git -C .deps/libdatachannel apply ../../patches/libdatachannel-v0.23.2-p2pdc.patch
PKG_CONFIG_PATH="$PWD/.deps/libnice-tls-install/lib/pkgconfig" \
cmake -S .deps/libdatachannel -B .deps/build -G Ninja \
  -DUSE_NICE=ON -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/.deps/install" \
  -DCMAKE_INSTALL_RPATH="$PWD/.deps/libnice-tls-install/lib"
cmake --build .deps/build --parallel 1 --target datachannel
cmake --install .deps/build
cmake -S . -B build-native -G Ninja -DP2PDC_ENABLE_NATIVE_TRANSPORT=ON \
  -DP2PDC_TEST_NATIVE_TRANSPORT=ON -DP2PDC_TEST_OPENDHT=ON \
  -DLibDataChannel_DIR="$PWD/.deps/install/lib/cmake/LibDataChannel"
cmake --build build-native --parallel 1
ctest --test-dir build-native --parallel 1 --output-on-failure
```

On Linux, use `nice -n 15 ionice -c 3` for builds and expensive checks, and
avoid running them concurrently. These local dependency builds embed their
absolute install path; rebuild them if the checkout is moved. The patch caps the backend worker pool at four
threads and changes local SCTP queue admission; it does not make local acceptance
a guarantee of remote delivery. It also avoids a libnice 0.1.24 receive-detach
bug that can call a null callback during connection cleanup. Rebuild and
reinstall the patched dependency when updating this library; native connection
creation checks that the updated patch is present. With libnice, the patch
requires headers and a runtime at least version 0.1.24; a build against older
headers is rejected because it could later load an affected newer runtime.
On Linux, `native_safe_nice_detach` checks the actual calls made during a local
connection teardown; it skips when libdatachannel uses another ICE backend.
The patch and native adapter remain experimental and require further integration testing.
The adapter bounds queued inbound payloads using per-connection limits and a
shareable budget; this does not bound WebRTC's own buffers, allocator overhead,
temporary application-envelope and browser/Wasm copies, or other executor tasks.
The budget is not a bound on peak process memory. A remote close can discard payloads still waiting for
delivery. These are internal primitives, not yet a stable public interface.
An internal native link composes the adapter with reliable fragmentation and
reassembly. It has no authenticated signaling or peer verification, and peers
currently need to agree on logical message limits outside the link.
Signed challenge/confirmation primitives exist for a future authenticated
handshake. A local WebRTC test derives its context from verified, directly
exchanged signed offer and answer records and verifies signed ICE candidates.
Local integration tests exercise two isolated OpenDHT nodes and authenticated
WebRTC message exchange, including an internal preauthorized native attempt
that owns the signaling and handshake components. End-of-candidates signaling is not
yet defined. Alpha signaling has bounded SDP and candidate sizes and does not
chunk oversized records; see [signaling limits](docs/DISCOVERY_SIGNALING.md#signaling).

A local integration test also feeds public discovery admission into an internal
outgoing attempt and exchanges authenticated bytes with an explicitly authorized
internal responder. The experimental public outgoing connection is owned by the
node and closes before node shutdown. It allows up to eight retained connections
per node; multiple attempts and a listener may share a lease. A connection reports observed direct or TURN route status;
unknown status does not establish that no relay was used. The experimental
`Connection::max_message_size()` and browser handle `maxMessageSize()` expose
the authenticated, negotiated maximum application message size. They return
no value before authentication or after failure or closure; suspension retains
the value. The limit is not a writable or delivery guarantee. Applications must
split and reassemble updates above the negotiated limit using their own protocol
and observe send backpressure. Native `ConnectOptions::max_message_bytes` sets
the local receive limit from 1 byte to 4 MiB, with a 256 KiB default; it must be
no larger than `max_pending_bytes`. Browser connection options similarly accept
`maxMessageBytes` from 1 to 4194304. The peer handshake uses the smaller limit
in both directions. Rebuild the WebAssembly bridge when updating the JavaScript
wrapper; mismatched versions reject connection creation.
The experimental
`Node::listen` reports one authenticated incoming request per listener;
`Node::accept` explicitly authorizes it. Closing the listener rejects it, and
expired or stale requests cannot be accepted. One listener per lease can be
active; after it is closed or accepted, another listener may use the lease while
existing connections remain live. A bounded in-memory replay history persists
across those listeners; later listeners must use the same discovery lifetime and
clock-skew policy. Unknown peers may be missed if discovery has not caught up.
When opposite-direction attempts overlap for the same peer and capability, a
stable ordering of peer signing keys and instance IDs selects the preferred
offerer. The nonpreferred attempt is superseded; accepting an incoming request
still requires application consent. Same-direction attempts remain independent.
The same C++ collision and cancellation tests pass on Linux, Android (two peers
on one device), and in Chromium and Firefox WebAssembly. Mixed Linux/browser
tests cover simultaneous dialing, rejection, and both cancellation directions.
Closing a pending request or connection attempts to notify its peer; DHT
signaling is best effort, so the peer can still time out if notification is lost.

The unpatched native TLS restriction is based on the pinned libdatachannel's
[RFC 5245 agent configuration](https://github.com/paullouisageneau/libdatachannel/blob/9e6a13abbb6846c003d817d0387b6706466e2b03/src/impl/icetransport.cpp)
and libnice's [TURN socket creation](https://github.com/libnice/libnice/blob/af62014d520ac09c8fe1974bda1b253114c68843/agent/agent.c).
An isolated probe against a plaintext-only coturn server gathered relay
candidates when configured for both TCP and TLS. This is not a TLS certificate
test; it demonstrates why unpatched native TLS is rejected. The local patches
add a real TLS connection with peer certificate and hostname verification.
See [Linux TURN/TLS tests](docs/TURN_TLS_TESTING.md) for the controlled
certificate, credential, relay, and socket regression checks.

An internal native discovery watcher and caller-driven announcement lease also
work against isolated OpenDHT nodes. Each put uses a fresh DHT value ID. The
default rotating protocol moves announcements to time-windowed keys and bounds
attempts per window; the legacy fixed-key protocol bounds attempts per lease.
Application-level expiry alone does not prevent old values from consuming DHT
lookup limits.
Native and browser discovery catch-up collapse byte-identical retries across
value IDs while limiting the number inspected. Live discovery also skips copies
that are still queued, and native watch batches skip identical content. Different
refreshes and sufficiently large batches can still exhaust their record limits.
The native public watcher bounds raw lookup records at four per configured peer,
with a minimum of 64 and maximum of 1024; memory and pending-event limits can
make discovery incomplete sooner. Exceeding a limit reports a resource error.
Native `WatchOptions` and `AnnounceOptions` default to
`DiscoveryProtocol::RotatingKeysV2`. Both handles for a connection must use the
same protocol; a mismatch is rejected. Browser discovery and `BrowserNode` also
default to rotating keys; the node's announcement handles inherit its watch's
choice and reject conflicting overrides. For compatibility, native callers can
select `FixedKeyV1` on both handles and browser callers can set
`watch: { windowed: false }` on the node. The protocols use different keys and
cannot discover one another. A rotating
watch may report catch-up again as its windows change, and its time policy must
fit within eight windows. Publication attempts, including retries, are bounded
per window. Local native/browser tests exchanged authenticated data in both
connection directions using rotating discovery and an isolated OpenDHT proxy.
An internal scheduled cycle can drive a watcher, lease, or both with an injected
timer scheduler, including bounded retry delays. Public announcement leases allow
at most 32 publication attempts per fixed-key lease or rotating-key window. A
node admits at most 16 valid announcement attempts in any rolling five minutes;
fixed-key announcements also have a 16-attempt node-lifetime limit. These
per-node limits do not bound repeated node creation.

The optional `p2pdc_native_turn_tests` target exercises the native adapter and
public connections against a separately supplied controlled TURN server. It
verifies forced-relay exchange, invalid credentials, direct connectivity in
automatic mode, and an automatic peer connecting to a relay-only peer. It requires
`P2PDC_TEST_TURN_HOST`, `P2PDC_TEST_TURN_USER`, and
`P2PDC_TEST_TURN_PASSWORD`, and expects TCP TURN on port 34789. It is excluded
from the default build and test suite. Use disposable credentials and a private
test server; the tests include intentionally incorrect credentials. On Linux,
`bash tests/run_native_public_turn_smoke.sh` runs the public forced-relay,
mixed-policy, and invalid-credential cases against an isolated local server.

The separate `p2pdc_native_auto_turn_tests` target checks two automatic peers
with direct traffic blocked. On Linux, `tests/run_isolated_auto_turn.sh` creates
an unprivileged, temporary user and network namespace with its own interfaces,
firewall and disposable local TURN server. It verifies that direct connectivity
fails without TURN, succeeds through TURN with valid credentials, reports the
selected relay, and fails with invalid credentials. The script requires `unshare`,
`ip`, `nft`, Python and `turnserver`; build the target first and run it serially.
Run it with `browser` instead of the default `native` argument to exercise two
Chromium peers under the same restrictions. The browser test additionally needs
Chromium and Python's `websocket-client` package; it uses real-time DevTools
polling. These are isolated transport tests, not internet or mobile-network tests.
Build `p2pdc_browser_transport_smoke` and run the script with `browser-cpp` to
test two C++-driven WebAssembly transports through a required local TURN relay.
The C++ raw WebRTC adapter is used by the public browser connection API.
The internal C++ browser attempt combines that adapter with signed signaling and
authenticated framed messages. Local tests exercise direct and required-TURN
connections with in-memory signaling, and a separate test exchanges signaling
through an OpenDHT proxy. Those lower-level tests use preauthorized peers;
the public API also performs discovery and application consent. The public
browser API has additionally passed direct, automatic TURN fallback, and
required-TURN tests with proxy discovery; an isolated firewall blocked direct
traffic in the TURN tests, which checked the selected route and retained TURN history. Build
`p2pdc_browser_public_connection_smoke` and run
`bash tests/run_browser_public_connection_smoke.sh` or append `turn` or
`fallback` for the relay tests. These tests do not validate internet or mobile networks.

For Android builds, device checks, and remaining mobile requirements, see
[Android cross-build status](docs/ANDROID.md). Current Firefox and Chromium
checks are described in [browser testing](docs/BROWSER_TESTING.md).

## WebAssembly

See [WebAssembly implementation notes](docs/WEBASSEMBLY_INTERNALS.md) for the
Emscripten build, internal adapters, and development tests. See
[browser testing](docs/BROWSER_TESTING.md) for current public-interface checks.

## Lifecycle

Node shutdown closes owned operations, joins its OpenDHT runner, and drains
admitted library backend tasks before delivering the final callback. The caller
must supply the documented nonblocking, nonthrowing executor. This barrier does
not wait for process-wide WebRTC cleanup or cover an abandoned node; preserve
the documented lifetime requirements for those cases. Callback ordering and
nested backend work have targeted tests, but shutdown under every platform's
network and scheduler behavior remains unverified.
