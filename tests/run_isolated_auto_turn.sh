#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-native}
if [[ $# -gt 1 || ( "$mode" != native && "$mode" != browser && "$mode" != browser-cpp ) ]]; then
    printf 'Usage: %s [native|browser|browser-cpp]\n' "$0" >&2
    exit 1
fi
binary="$root/build-native/p2pdc_native_auto_turn_tests"
if [[ "$mode" == native && ! -x "$binary" ]]; then
    printf 'Build p2pdc_native_auto_turn_tests first.\n' >&2
    exit 1
fi
if [[ "$mode" == browser || "$mode" == browser-cpp ]]; then
    command -v chromium >/dev/null
    python3 -c 'import websocket'
fi

# The interfaces, firewall, server and disposable credentials exist only in
# this unprivileged user/network namespace. No host firewall rules are changed.
unshare --user --map-root-user --net --fork bash -s -- "$binary" "$mode" "$root" <<'INNER'
set -euo pipefail
binary=$1
mode=$2
root=$3
ip link set lo up
ip link add dev p2pa type veth peer name p2pb
ip addr add 10.44.0.1/24 dev p2pa
ip addr add 10.44.0.2/24 dev p2pb
ip link set p2pa up
ip link set p2pb up
ip route add default via 10.44.0.2 dev p2pa
nft add table inet p2pdc_test
nft 'add chain inet p2pdc_test output { type filter hook output priority 0; policy accept; }'
if [[ "$mode" == browser || "$mode" == browser-cpp ]]; then
    nft add rule inet p2pdc_test output ip saddr 127.0.0.0/8 ip daddr 127.0.0.0/8 meta l4proto tcp accept
fi
nft add rule inet p2pdc_test output udp sport != 10000-10099 udp dport != 10000-10099 counter drop
nft add rule inet p2pdc_test output tcp sport != 34789 tcp dport != 34789 counter drop

test_dir=$(mktemp -d /tmp/p2pdc-isolated-turn.XXXXXX)
turnserver -c /dev/null --no-cli --no-tls --no-multicast-peers \
    --relay-threads=0 --listening-ip=10.44.0.1 --relay-ip=10.44.0.1 --listening-port=34789 \
    --min-port=10000 --max-port=10099 --lt-cred-mech --realm=p2pdc.test \
    --user=p2pdc:temporary-test-password --no-stdout-log --log-file="$test_dir/turn.log" \
    --pidfile="$test_dir/turn.pid" &
turn_pid=$!
pids=("$turn_pid")
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
for attempt in {1..100}; do
    if (echo >/dev/tcp/10.44.0.1/34789) 2>/dev/null; then break; fi
    if ! kill -0 "$turn_pid" 2>/dev/null; then echo 'TURN server exited during startup' >&2; exit 1; fi
    sleep 0.05
done
if [[ "$mode" == native ]]; then
    export P2PDC_TEST_TURN_HOST=10.44.0.1 P2PDC_TEST_TURN_USER=p2pdc
    export P2PDC_TEST_TURN_PASSWORD=temporary-test-password
    "$binary" --gtest_filter='NativeTurnFallback.*'
else
    python3 -m http.server 18765 --bind 127.0.0.1 --directory "$root" >"$test_dir/http.log" 2>&1 &
    pids+=("$!")
    chromium --headless --no-sandbox --disable-gpu --disable-dev-shm-usage --renderer-process-limit=1 \
        --disable-extensions --no-first-run --disable-features=WebRtcHideLocalIpsWithMdns \
        --remote-debugging-port=19222 --remote-allow-origins='*' --user-data-dir="$test_dir/chrome" \
        about:blank >"$test_dir/chromium.log" 2>&1 &
    pids+=("$!")
    for attempt in {1..100}; do
        if (echo >/dev/tcp/127.0.0.1/19222) 2>/dev/null &&
           (echo >/dev/tcp/127.0.0.1/18765) 2>/dev/null; then break; fi
        sleep 0.05
    done
    if [[ "$mode" == browser-cpp ]]; then
        page=http://127.0.0.1:18765/tests/browser_transport_smoke.html
        python3 "$root/tests/browser_cdp_smoke.py" "$page?turn=1"
    else
        page=http://127.0.0.1:18765/tests/browser_auto_turn_isolated.html
        python3 "$root/tests/browser_cdp_smoke.py" "$page?mode=blocked" "$page?mode=turn" "$page?mode=bad"
    fi
fi
nft -j list chain inet p2pdc_test output | python3 -c '
import json, sys
rules = json.load(sys.stdin)["nftables"]
packets = sum(expr["counter"]["packets"] for item in rules
              for expr in item.get("rule", {}).get("expr", []) if "counter" in expr)
if packets == 0:
    raise SystemExit("network filter did not block any direct packets")
print(f"Isolated network filter blocked {packets} direct packets")
'
INNER
