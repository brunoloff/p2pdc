#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-collision}
role=${2:-native-offerer}
browser=${P2PDC_TEST_BROWSER:-chromium}
if [[ $# -gt 2 || ( "$mode" != collision && "$mode" != reject && "$mode" != cancel-accepted && "$mode" != cancel-dial ) ||
      ( "$role" != native-offerer && "$role" != browser-offerer ) ||
      ( "$browser" != chromium && "$browser" != firefox ) ]]; then
    echo "Usage: $0 [collision|reject|cancel-accepted|cancel-dial] [native-offerer|browser-offerer]" >&2
    exit 1
fi
for command in unshare ip dhtnode curl python3 setsid; do command -v "$command" >/dev/null; done
if [[ "$browser" == chromium ]]; then command -v chromium >/dev/null; python3 -c 'import websocket'; fi
if [[ "$browser" == firefox ]]; then python3 -c 'import playwright.sync_api'; fi
unshare --user --map-root-user --net --fork bash -s -- "$root" "$mode" "$role" "$browser" <<'INNER'
set -euo pipefail
root=$1 mode=$2 role=$3 browser=$4
ip link set lo up
ip link add p2pdc-test0 type veth peer name p2pdc-test1
ip addr add 10.42.0.1/24 dev p2pdc-test0
ip addr add 10.42.0.2/24 dev p2pdc-test1
ip link set p2pdc-test0 up
ip link set p2pdc-test1 up
ip route add default via 10.42.0.2 dev p2pdc-test0
test_dir=$(mktemp -d /tmp/p2pdc-mixed-race.XXXXXX)
dht_pid= native_pid= server_pid= browser_pid=
cleanup() {
    test_exit=$?
    for pid in "$browser_pid" "$server_pid" "$native_pid" "$dht_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    if (( test_exit != 0 )); then
        for log in native dht server chromium; do
            if [[ -f "$test_dir/$log.log" ]]; then tail -40 "$test_dir/$log.log" >&2; fi
        done
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
setsid bash -c 'tail -f /dev/null | dhtnode -n 873198 -p 49411 --proxyserver 49412' >"$test_dir/dht.log" 2>&1 &
dht_pid=$!
for attempt in {1..100}; do
    if curl -sS -o /dev/null http://127.0.0.1:49412/ 2>/dev/null; then break; fi
    if ! kill -0 "$dht_pid" 2>/dev/null; then echo 'DHT proxy failed to start' >&2; exit 1; fi
    sleep 0.1
done
native_role=offerer browser_role=answerer
if [[ "$role" == browser-offerer ]]; then native_role=answerer; browser_role=offerer; fi
setsid "$root/build-native/p2pdc_native_mixed_race" "$mode" "$native_role" >"$test_dir/native.log" 2>&1 &
native_pid=$!
setsid python3 -m http.server 49070 --bind 127.0.0.1 --directory "$root" >"$test_dir/server.log" 2>&1 &
server_pid=$!
if [[ "$browser" == chromium ]]; then
    XDG_CONFIG_HOME="$test_dir/config" setsid chromium --headless --no-sandbox --disable-gpu \
        --disable-dev-shm-usage --disable-features=WebRtcHideLocalIpsWithMdns \
        --renderer-process-limit=1 --remote-debugging-port=19222 \
        --remote-allow-origins='*' --user-data-dir="$test_dir/profile" about:blank >"$test_dir/chromium.log" 2>&1 &
    browser_pid=$!
fi
for attempt in {1..100}; do
    if curl -fsS http://127.0.0.1:49070/tests/browser_public_connection_smoke.html >/dev/null 2>&1 && \
       { [[ "$browser" == firefox ]] || curl -fsS http://127.0.0.1:19222/json >/dev/null 2>&1; }; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null || ! kill -0 "$native_pid" 2>/dev/null || \
       { [[ -n "$browser_pid" ]] && ! kill -0 "$browser_pid" 2>/dev/null; }; then
        echo 'test process failed to start' >&2; exit 1
    fi
    sleep 0.1
done
sleep 2
page="http://127.0.0.1:49070/tests/browser_public_connection_smoke.html?mixed=$mode&role=$browser_role"
if [[ "$browser" == firefox ]]; then
    python3 "$root/tests/browser_playwright_smoke.py" --timeout 45 "$page"
else
    python3 "$root/tests/browser_cdp_smoke.py" --timeout 45 "$page"
fi
wait "$native_pid"
native_pid=
if ! grep -q '^PASS$' "$test_dir/native.log"; then echo 'native peer did not confirm the race' >&2; exit 1; fi
echo "Mixed native/browser $mode passed ($role)"
INNER
