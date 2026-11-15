#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
peer="$root/build-native/p2pdc_browser_record_native_peer"
if [[ ! -x "$peer" || ! -f "$root/build-wasm-auth/p2pdc_browser_record_store_smoke.js" ]]; then
    printf 'Build the native record peer and WebAssembly record-store smoke target first.\n' >&2
    exit 1
fi
for command in unshare ip dhtnode chromium curl python3 setsid node; do command -v "$command" >/dev/null; done
python3 -c 'import websocket'
node --check "$root/build-wasm-auth/p2pdc_browser_record_store_smoke.js"

# An isolated network namespace keeps fixed test ports away from host services.
unshare --user --map-root-user --net --fork bash -s -- "$root" "$peer" <<'INNER'
set -euo pipefail
root=$1
peer=$2
ip link set lo up
ip link add p2pdc-test0 type veth peer name p2pdc-test1
ip addr add 10.42.0.1/24 dev p2pdc-test0
ip addr add 10.42.0.2/24 dev p2pdc-test1
ip link set p2pdc-test0 up
ip link set p2pdc-test1 up
ip route add default via 10.42.0.2 dev p2pdc-test0
test_dir=$(mktemp -d /tmp/p2pdc-browser-record.XXXXXX)
dht_pid= peer_pid= server_pid= browser_pid=
cleanup() {
    status=$?
    for pid in "$browser_pid" "$server_pid" "$peer_pid" "$dht_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    if (( status != 0 )); then
        for log in peer dht server chromium; do
            if [[ -f "$test_dir/$log.log" ]]; then tail -40 "$test_dir/$log.log" >&2; fi
        done
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49411 --proxyserver 49412' >"$test_dir/dht.log" 2>&1 &
dht_pid=$!
for attempt in {1..100}; do
    if curl -sS -o /dev/null http://127.0.0.1:49412/ 2>/dev/null; then break; fi
    if ! kill -0 "$dht_pid" 2>/dev/null; then echo 'DHT proxy failed to start' >&2; exit 1; fi
    sleep 0.1
done
setsid "$peer" >"$test_dir/peer.log" 2>&1 &
peer_pid=$!
for attempt in {1..150}; do
    if grep -q '^READY$' "$test_dir/peer.log"; then break; fi
    if ! kill -0 "$peer_pid" 2>/dev/null; then echo 'native peer failed to start' >&2; exit 1; fi
    sleep 0.1
done
if ! grep -q '^READY$' "$test_dir/peer.log"; then echo 'native peer startup timed out' >&2; exit 1; fi
setsid python3 -m http.server 49070 --bind 127.0.0.1 --directory "$root" >"$test_dir/server.log" 2>&1 &
server_pid=$!
XDG_CONFIG_HOME="$test_dir/config" setsid chromium --headless --no-sandbox --disable-gpu \
    --disable-dev-shm-usage --disable-features=WebRtcHideLocalIpsWithMdns \
    --renderer-process-limit=1 --remote-debugging-port=19222 \
    --remote-allow-origins='*' --user-data-dir="$test_dir/profile" about:blank >"$test_dir/chromium.log" 2>&1 &
browser_pid=$!
for attempt in {1..100}; do
    if curl -fsS http://127.0.0.1:19222/json >/dev/null 2>&1 && \
       curl -fsS http://127.0.0.1:49070/tests/browser_record_store_smoke.html >/dev/null 2>&1; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null || ! kill -0 "$browser_pid" 2>/dev/null; then
        echo 'browser or web server failed to start' >&2; exit 1
    fi
    sleep 0.1
done
python3 "$root/tests/browser_cdp_smoke.py" --timeout 25 \
    http://127.0.0.1:49070/tests/browser_transport_smoke.html \
    http://127.0.0.1:49070/tests/browser_record_store_smoke.html
for attempt in {1..100}; do
    if ! kill -0 "$peer_pid" 2>/dev/null; then break; fi
    sleep 0.1
done
if kill -0 "$peer_pid" 2>/dev/null; then echo 'native peer timed out' >&2; exit 1; fi
wait "$peer_pid"
if ! grep -q '^PASS$' "$test_dir/peer.log"; then echo 'native peer did not verify browser record' >&2; exit 1; fi
echo 'Native and WebAssembly record stores exchanged both directions'
INNER
