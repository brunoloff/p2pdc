# Browser smoke tests

The public C++ connection smoke test can run in Chromium or Firefox. It starts
an isolated local OpenDHT network and uses disposable test identities; it does
not establish internet reachability or mobile-browser support.

The first alpha targets the versions of Chromium and Firefox listed in
[dependency versions](DEPENDENCIES.md). Safari and mobile browsers remain
untested. The local WebKitGTK 2.52.6 build exposed no `RTCPeerConnection`, so
it could only test the library's `Unsupported` result, not a connection.

Chromium uses the local Chrome DevTools Protocol driver. Firefox uses Python
Playwright and its matching Firefox build. For a project-local browser install:

```sh
export PLAYWRIGHT_BROWSERS_PATH="$PWD/.deps/playwright-browsers"
playwright install firefox
P2PDC_TEST_BROWSER=firefox bash tests/run_browser_public_connection_smoke.sh direct
P2PDC_TEST_BROWSER=firefox bash tests/run_browser_native_public_smoke.sh native-offerer
P2PDC_TEST_BROWSER=firefox bash tests/run_browser_native_public_smoke.sh browser-offerer
```

With a quiet OpenDHT listen stream, Firefox may not expose response headers
before the first record. The browser record store falls back to bounded
polling if stream startup takes two seconds. Polling can miss short-lived
records between snapshots and does not establish DHT completeness.

The direct test passed on local Chromium and Firefox; Firefox also exchanged
data with a Linux peer in both connection roles. Firefox automatic TURN
fallback passed with direct packets blocked by the isolated filter. A Firefox
relay-required
run exchanged data and reported TURN, but the test's network filter recorded no
blocked direct packets, so the complete relay test did not pass. A successful
result on other Firefox versions and other browser engines remains to be
established.

Mixed Linux/browser tests cover simultaneous dialing and rejection, cancellation
of a pending dial, and cancellation immediately after acceptance. The latter
three run with each platform as the offerer. These use a disposable local DHT
and do not demonstrate behavior across internet networks. For either browser:

```sh
export PLAYWRIGHT_BROWSERS_PATH="$PWD/.deps/playwright-browsers"
export P2PDC_TEST_BROWSER=firefox  # or chromium
bash tests/run_browser_native_race_smoke.sh collision native-offerer
for mode in reject cancel-dial cancel-accepted; do
  for role in native-offerer browser-offerer; do
    bash tests/run_browser_native_race_smoke.sh "$mode" "$role"
  done
done
```

The isolated Firefox and Chromium TURN-over-TLS tests use a disposable CA, a
separate server certificate, and a temporary browser profile. The trusted tests
exchange data over relayed connections and coturn records authenticated TLS
allocations. The untrusted-CA and wrong-hostname controls have ICE errors and
no allocations. A separate pair checks a valid time-limited TURN REST
credential and an expired credential; coturn explicitly reports the expired
timestamp. These tests do not establish certificate behavior on other browser
versions or Android.

```sh
export PLAYWRIGHT_BROWSERS_PATH="$PWD/.deps/playwright-browsers"
for browser in firefox chromium; do
  for mode in tls tls-untrusted tls-name tls-timed tls-expired; do
    P2PDC_TEST_BROWSER="$browser" bash tests/run_browser_public_connection_smoke.sh "$mode"
  done
done
```

The fixture requires `openssl`, `certutil`, and `turnserver`. Chromium's TLS
tests also need `mount` and an unprivileged mount namespace to isolate any
existing legacy NSS database. Its synthetic
valid REST credential expires at Unix timestamp `2000000000`; regenerate its
HMAC before that date. No system trust store is changed.

The shared C++ echo example has also exchanged messages over the internet
between Android and Chromium (browser initiator) and between Android and
Firefox (phone initiator), using Jami's HTTPS OpenDHT proxy and public DHT
network. Both direct and relay-required exchanges passed in those combinations.
On Firefox, the route became `Unknown` during shutdown while TURN history
remained `Used`; the relay check verifies the route at the echo exchange.

With an isolated filter blocking direct ICE checks on the browser side,
automatic fallback to a phone's TURN allocation passed with the phone as
initiator in Chromium and Firefox. Firefox also exchanged data with the browser
as initiator. Before the signaling change described below, browser-initiated
attempts intermittently failed in both engines before the phone's relay
candidate arrived. The no-TURN control must also observe blocked direct checks;
some runs did not meet that condition and are not complete fallback passes.
In one failed Firefox attempt, ICE failed about 3.6 seconds after its first
remote candidate, before any phone relay candidate was observed. In another
failed attempt, temporary Android instrumentation showed relay candidates were
generated within 0.15 seconds of the answer, while the answer's DHT publication
completion took about eight seconds. The native outbox then published signaling
records serially, so it held those candidates while Firefox timed out. The
signaling pipe now allows bounded concurrent publications. After this change,
Firefox and Chromium each passed a browser-initiated fallback test with blocked
direct checks and a failing no-TURN control; an additional Firefox run passed
before rebuilding the browser side. Both browsers also passed the corresponding
phone-initiated test and no-TURN control. More varied networks remain untested.

To repeat the opt-in browser/phone test, build `p2pdc_echo` in
`build-wasm-auth`, prepare the Android payload as in [Android testing](ANDROID.md),
and connect the phone and computer through different networks. This command
uses Jami's proxy and Google STUN, which can observe network addresses:

```sh
P2PDC_TEST_NATIVE_RUNNER="$PWD/tests/browser_internet_echo.py" \
P2PDC_TEST_BROWSER_PROXY=https://dhtproxy.jami.net \
P2PDC_TEST_BROWSER_ENGINE=chromium \
bash tests/run_android_internet_echo.sh DEVICE_SERIAL phone-responder direct
```

Set `P2PDC_TEST_BROWSER_ENGINE=firefox` and `PLAYWRIGHT_BROWSERS_PATH` as above
to use Firefox. The test serves only the example and its Wasm artifacts on
loopback. To test TURN, set the TURN environment variables described in the
Android instructions and replace `direct` with `relay`. The caller selects the
proxy and TURN service; the library does not select either automatically.
