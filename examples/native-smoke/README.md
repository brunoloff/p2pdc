# Native binary echo smoke

This standalone executable tests the native libdatachannel backend, with two peers
and in-process signaling. It does not test OpenDHT discovery, browser interop, the
library protocol, or connectivity across separate networks. It creates no server
processes and logs neither TURN credentials nor candidate addresses.

## Build and run

From the library root:

```sh
cmake -S examples/native-smoke -B /tmp/p2pdc-native-smoke-build -G Ninja \
  -DCMAKE_PREFIX_PATH=/tmp/p2pdc-deps/install
cmake --build /tmp/p2pdc-native-smoke-build --parallel 1
ctest --test-dir /tmp/p2pdc-native-smoke-build --output-on-failure
/tmp/p2pdc-native-smoke-build/native_smoke direct
```

For an existing controlled TURN server, set these environment variables to its
actual configuration: `P2PDC_TURN_HOST`, `P2PDC_TURN_PORT`,
`P2PDC_TURN_USERNAME`, and `P2PDC_TURN_PASSWORD`. Credentials are read from the
environment rather than command-line arguments. Then run:

```sh
/tmp/p2pdc-native-smoke-build/native_smoke turn-udp
/tmp/p2pdc-native-smoke-build/native_smoke turn-tcp
```

Each run waits up to 20 seconds for an exact binary echo containing zero and
non-ASCII bytes. Exit 0 means the echo matched and the selected pair has the
requested route category; exit 1 means echo or route verification failed; exit 2
means invalid configuration or an exception. The selected pair can still be
unknown after a failed connection. The test harness imposes a 30-second process
limit for its direct integration test; use an external timeout for manual runs
if a backend stalls during teardown.

`TurnRelayed` requires either selected candidate to be relayed. `Direct` requires
an observed pair with known host or server-reflexive candidates; a peer-reflexive
candidate is ambiguous. `require_local_relay=met|unmet`
is a separate observation. Both TURN peers request `TransportPolicy::Relay`, but
libdatachannel/libnice can still select a local host candidate and a remote relay.
A successful TURN smoke therefore does **not** prove RequireLocalRelay. The
production adapter must check the local selected candidate and reject policy
violations. These smoke commands deliberately report this distinction.

TURN TCP refers to the control connection to TURN; relayed packets use UDP. Build
with libnice for TURN TCP support. TLS is outside this executable's scope.

## Validated isolated dependency build

The following libdatachannel commands were executed successfully on the current
Manjaro host with CMake, Ninja, GCC, OpenSSL, and libnice development files already
installed. Use a fresh destination when reproducing the clone.

```sh
git clone --depth 1 --branch v0.23.2 --recurse-submodules --shallow-submodules \
  https://github.com/paullouisageneau/libdatachannel.git /tmp/p2pdc-deps/libdatachannel
cmake -S /tmp/p2pdc-deps/libdatachannel \
  -B /tmp/p2pdc-deps/build-datachannel -G Ninja \
  -DCMAKE_INSTALL_PREFIX=/tmp/p2pdc-deps/install \
  -DCMAKE_BUILD_TYPE=Release -DUSE_NICE=ON \
  -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON
cmake --build /tmp/p2pdc-deps/build-datachannel --parallel 1
cmake --install /tmp/p2pdc-deps/build-datachannel
```

Validated commit: `9e6a13abbb6846c003d817d0387b6706466e2b03` (v0.23.2).
No upstream library files were modified. The project also needs GoogleTest for
its configuration tests. The system package database listed `libdatachannel`,
`opendht`, and `coturn`; packaged libdatachannel depended on libjuice, so that
package alone was not used for the TURN TCP validation.

## Controlled TURN server and observed limits

A temporary coturn 4.7.0 was built with the following commands. Its newer master
required an unavailable libmicrohttpd dependency, whereas this tag configured
and built successfully with the existing OpenSSL/libevent/SQLite development files.

```sh
git clone --depth 1 --branch 4.7.0 https://github.com/coturn/coturn.git \
  /tmp/p2pdc-deps/coturn
cmake -S /tmp/p2pdc-deps/coturn -B /tmp/p2pdc-deps/build-coturn-stable \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/tmp/p2pdc-deps/install
cmake --build /tmp/p2pdc-deps/build-coturn-stable --parallel 1
```

Run a controlled server in the foreground in a separate terminal, using your own
private test interface and credentials. `P2PDC_TURN_REALM` and
`P2PDC_TURN_RELAY_MIN`/`P2PDC_TURN_RELAY_MAX` must also be set for this command.
This command intentionally does not start a background or system service. Stop it
with Ctrl-C immediately after the tests; credentials appear in this server's
process arguments, so use disposable test credentials.

```sh
/tmp/p2pdc-deps/build-coturn-stable/bin/turnserver -n \
  --listening-ip="$P2PDC_TURN_HOST" --relay-ip="$P2PDC_TURN_HOST" \
  --listening-port="$P2PDC_TURN_PORT" \
  --min-port="$P2PDC_TURN_RELAY_MIN" --max-port="$P2PDC_TURN_RELAY_MAX" \
  --realm="$P2PDC_TURN_REALM" \
  --user="$P2PDC_TURN_USERNAME:$P2PDC_TURN_PASSWORD" \
  --lt-cred-mech --fingerprint --no-multicast-peers --no-cli \
  --no-tls --no-dtls --relay-threads=1 \
  --log-file=/tmp/p2pdc-native-turn.log --pidfile=/tmp/p2pdc-native-turn.pid
```

Observed on this host: direct echo passed with host/host; TURN UDP and TCP passed
against the machine's private network interface with host/relay. A loopback TURN
server produced no libnice UDP candidates, despite coturn's own UDP client working;
TURN TCP against loopback did pass with relay/relay. Use an actual private interface
for reproducing UDP tests. Do not infer all-interface or real-NAT coverage from
these same-machine results. All servers started during validation were stopped.

## Source checks

From the library root, with the standalone build configured as above:

```sh
clang-format --dry-run --Werror examples/native-smoke/*.{hpp,cpp}
clang-tidy -p /tmp/p2pdc-native-smoke-build \
  --checks='clang-analyzer-*,bugprone-*' --warnings-as-errors='*' \
  examples/native-smoke/config.cpp examples/native-smoke/smoke.cpp \
  examples/native-smoke/main.cpp examples/native-smoke/config_test.cpp
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only \
  -I/tmp/p2pdc-deps/install/include examples/native-smoke/*.cpp
```
