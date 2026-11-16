# Shared C++ echo peer

`echo_peer.cpp` is an application-level example using only the public
`p2pdc` C++ API. The same source is compiled by the native and WebAssembly
integration tests. It sends a small binary message and echoes it back, delivers
route observations to a callback, and provides names for `Unknown`, `Direct`,
and `TurnRelayed`. It does
not depend on Yjs, Yrs, or any particular document model.

Create one peer with `Role::Initiator` and one with `Role::Responder`. Supply
both with the same high-entropy discovery capability, and authorize each peer
with the other peer's independently verified signing public key. Provide the
native bootstrap settings and browser proxy URL in the same `DiscoveryConfig`;
each backend uses the settings appropriate to its platform. Supply STUN or
TURN settings in `IceConfig` if required by the network.

`EchoPeer::create`, `start`, and `shutdown` run on the node's serial executor.
The caller must retain the peer, executor, scheduler, and runtime until the
shutdown callback completes. The `signing_seed` and `recipient_secret` views are
borrowed only for `create`: the caller is responsible for storing, protecting,
and wiping the secret buffers. Callbacks must not throw. On an error, stop the
peer using `shutdown`; `on_echo` means the initiator received the echo and the
responder received its acknowledgement. This confirms the example exchange,
not that a remote application persisted anything.

The example reports a send failure if the channel is temporarily backpressured.
A real application should retain its own message and retry when `on_writable`
is delivered. This small example handles one connection and is not a full
application, identity-management system, or document synchronization protocol.
The [test driver](../../tests/echo_example_pair.cpp) shows how to create and
shut down two peers; its fixed test secrets are deliberately not suitable for
real use.

## Standalone program

Build the same application source for either target (the launcher in `bin/`
prefers `build-native-tls`, then `build-native`):

```sh
cmake --build build-native --target p2pdc_echo --parallel 1
cmake --build build-wasm-auth --target p2pdc_echo --parallel 1
```

The native executable reads its configuration from environment variables.
Generate a distinct signing seed and recipient secret for each peer; give both
peers the same high-entropy capability through a secure channel. For example,
`openssl rand -hex 32` generates one such 32-byte value. Set these variables on
each peer, replacing the placeholders with its own values:

```sh
export P2PDC_ECHO_ROLE=initiator              # use responder on the other peer
export P2PDC_ECHO_CAPABILITY='<shared 64-digit hex>'
export P2PDC_ECHO_SEED='<your signing seed, 64-digit hex>'
export P2PDC_ECHO_SECRET='<your recipient secret, 64-digit hex>'
export P2PDC_ECHO_PEER_KEY='<other peer signing public key, 64-digit hex>'
export P2PDC_ECHO_BOOTSTRAP_HOST='<OpenDHT bootstrap host>'
export P2PDC_ECHO_BOOTSTRAP_PORT=4222
export P2PDC_ECHO_NETWORK=0
./bin/p2pdc_echo
```

To obtain your public signing key before starting, set `P2PDC_ECHO_SEED` and
run `./bin/p2pdc_echo --public-key`. Exchange public keys independently
and verify them before authorizing a peer. Do not share either private secret.
The example times out after 60 seconds and exits after the echo and its
acknowledgement complete. Environment variables and browser page scripts can
expose input secrets; use disposable identities for this demonstration.

Optionally set `P2PDC_ECHO_STUN_HOST` and `P2PDC_ECHO_STUN_PORT`. For TURN, set
`P2PDC_ECHO_TURN_HOST`, `P2PDC_ECHO_TURN_PORT`, `P2PDC_ECHO_TURN_USER`,
`P2PDC_ECHO_TURN_PASSWORD`, and `P2PDC_ECHO_TURN_TRANSPORT` (`udp`, `tcp`, or
`tls`). `P2PDC_ECHO_RELAY_POLICY` is `automatic` by default or `required` to
require a local relay. The route callback reports the observed current route and
whether this endpoint has observed TURN use. A peer-reflexive route without a
known relay is `Unknown`; the other endpoint may have better evidence.

For the browser, serve the repository over HTTPS, or over HTTP on loopback for
local testing, and open `examples/echo/index.html`. The form loads the
`build-wasm-auth` output, accepts the same configuration, and can derive the
public signing key from a seed. Reload after deriving a key before starting a
session. Supply an HTTPS OpenDHT proxy reachable by the browser and configured
for the same network; a native bootstrap server alone is not a browser proxy.

## Isolated checks

To run the isolated local checks, configure the native build with
`P2PDC_TEST_OPENDHT=ON` and `P2PDC_TEST_NATIVE_TRANSPORT=ON`, and the
WebAssembly build with `P2PDC_BUILD_BROWSER_SECURITY_TEST=ON`. Build
`p2pdc_echo` for both targets, `p2pdc_native_transport_tests`, and
`p2pdc_browser_public_connection_smoke`, then run:

```sh
./build-native/p2pdc_native_transport_tests --gtest_filter='NativeEchoExampleTest.*:EchoExampleOptionsTest.*'
bash tests/run_browser_public_connection_smoke.sh echo
bash tests/run_browser_public_connection_smoke.sh echo-reject
bash tests/run_browser_public_connection_smoke.sh echo-turn
bash tests/run_browser_public_connection_smoke.sh echo-fallback
bash tests/run_native_public_turn_smoke.sh
bash tests/run_echo_standalone_native_smoke.sh
bash tests/run_echo_standalone_interop_smoke.sh browser-initiator direct
bash tests/run_echo_standalone_interop_smoke.sh native-initiator direct
bash tests/run_echo_standalone_interop_smoke.sh browser-initiator turn
bash tests/run_echo_standalone_interop_smoke.sh native-initiator turn
bash tests/run_echo_standalone_interop_smoke.sh browser-initiator fallback
bash tests/run_echo_standalone_interop_smoke.sh native-initiator fallback
```

The native TURN command also needs `p2pdc_native_turn_tests` built. The relay
tests need `turnserver`; the browser relay tests additionally need `nft`.
These tests use disposable deterministic identities and isolated local DHT
nodes; the browser tests also use a loopback proxy. They do not test internet
reachability or mobile devices. Never use the tests' fixed identities or
capability for a real session.
