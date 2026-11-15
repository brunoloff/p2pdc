#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
role=${1:-browser-initiator}
mode=${2:-direct}
if [[ $# -gt 2 || ( "$role" != browser-initiator && "$role" != native-initiator ) || \
      ( "$mode" != direct && "$mode" != turn && "$mode" != fallback ) ]]; then
    printf 'Usage: %s [browser-initiator|native-initiator] [direct|turn|fallback]\n' "$0" >&2
    exit 1
fi
for command in unshare ip dhtnode chromium curl python3 setsid timeout; do command -v "$command" >/dev/null; done
if [[ "$mode" != direct ]]; then command -v nft >/dev/null; command -v turnserver >/dev/null; fi
python3 -c 'import websocket'
unshare --user --map-root-user --net --fork bash -s -- "$root" "$role" "$mode" <<'INNER'
set -euo pipefail
root=$1
unset P2PDC_ECHO_STUN_HOST P2PDC_ECHO_STUN_PORT P2PDC_ECHO_TURN_HOST P2PDC_ECHO_TURN_PORT
unset P2PDC_ECHO_TURN_USER P2PDC_ECHO_TURN_PASSWORD P2PDC_ECHO_TURN_TRANSPORT P2PDC_ECHO_RELAY_POLICY
unset P2PDC_ECHO_PROXY
role=$2
mode=$3
ip link set lo up
ip link add p2pdc-test0 type veth peer name p2pdc-test1
ip addr add 10.42.0.1/24 dev p2pdc-test0
ip addr add 10.42.0.2/24 dev p2pdc-test1
ip link set p2pdc-test0 up
ip link set p2pdc-test1 up
ip route add default via 10.42.0.2 dev p2pdc-test0
if [[ "$mode" != direct ]]; then
    nft add table inet p2pdc_test
    nft 'add chain inet p2pdc_test output { type filter hook output priority 0; policy accept; }'
    nft add rule inet p2pdc_test output ip saddr 127.0.0.0/8 ip daddr 127.0.0.0/8 accept
    nft add rule inet p2pdc_test output udp sport != 10000-10099 udp dport != 10000-10099 counter drop
    nft add rule inet p2pdc_test output tcp sport != 34789 tcp dport != 34789 counter drop
fi
work_dir=$(mktemp -d /tmp/p2pdc-standalone-interop.XXXXXX)
dht_pid= extra_pid= peer_pid= server_pid= browser_pid= turn_pid=
cleanup() {
    test_exit=$?
    for pid in "$browser_pid" "$server_pid" "$peer_pid" "$extra_pid" "$dht_pid" "$turn_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    if (( test_exit != 0 )); then
        for log in peer dht extra server chromium turn; do
            if [[ -f "$work_dir/$log.log" ]]; then tail -50 "$work_dir/$log.log" >&2; fi
        done
    fi
    rm -rf -- "$work_dir"
}
trap cleanup EXIT
if [[ "$mode" != direct ]]; then
    setsid turnserver -c /dev/null --no-cli --no-tls --no-multicast-peers --relay-threads=0 \
        --listening-ip=10.42.0.1 --relay-ip=10.42.0.1 --listening-port=34789 \
        --min-port=10000 --max-port=10099 --lt-cred-mech --realm=p2pdc.test \
        --user=p2pdc:temporary-test-password --no-stdout-log --log-file="$work_dir/turn.log" \
        --pidfile="$work_dir/turn.pid" >"$work_dir/turn.out" 2>&1 &
    turn_pid=$!
    for attempt in {1..100}; do
        if (echo >/dev/tcp/10.42.0.1/34789) 2>/dev/null; then break; fi
        if ! kill -0 "$turn_pid" 2>/dev/null; then echo 'TURN server failed to start' >&2; exit 1; fi
        sleep 0.1
    done
    export P2PDC_ECHO_TURN_HOST=10.42.0.1 P2PDC_ECHO_TURN_PORT=34789
    export P2PDC_ECHO_TURN_USER=p2pdc P2PDC_ECHO_TURN_PASSWORD=temporary-test-password
    export P2PDC_ECHO_TURN_TRANSPORT=tcp P2PDC_ECHO_RELAY_POLICY=automatic
    if [[ "$mode" == turn ]]; then export P2PDC_ECHO_RELAY_POLICY=required; fi
fi
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49411 --proxyserver 49412' >"$work_dir/dht.log" 2>&1 &
dht_pid=$!
for attempt in {1..100}; do
    if curl -sS -o /dev/null http://127.0.0.1:49412/ 2>/dev/null; then break; fi
    if ! kill -0 "$dht_pid" 2>/dev/null; then echo 'DHT proxy failed to start' >&2; exit 1; fi
    sleep 0.1
done
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49413 -b 127.0.0.1:49411' >"$work_dir/extra.log" 2>&1 &
extra_pid=$!
seed_a=$(printf '01%.0s' {1..32})
seed_b=$(printf '02%.0s' {1..32})
secret_a=$(printf '03%.0s' {1..32})
secret_b=$(printf '04%.0s' {1..32})
capability=$(printf '05%.0s' {1..32})
key_a=$(P2PDC_ECHO_SEED="$seed_a" "$root/build-native/p2pdc_echo" --public-key)
key_b=$(P2PDC_ECHO_SEED="$seed_b" "$root/build-native/p2pdc_echo" --public-key)
native_role=responder native_seed=$seed_b native_secret=$secret_b native_key=$key_a
browser_role=initiator browser_key=$key_b
if [[ "$role" == native-initiator ]]; then
    native_role=initiator native_seed=$seed_a native_secret=$secret_a native_key=$key_b
    browser_role=responder browser_key=$key_a
fi
setsid python3 -m http.server 49070 --bind 127.0.0.1 --directory "$root" >"$work_dir/server.log" 2>&1 &
server_pid=$!
XDG_CONFIG_HOME="$work_dir/config" setsid chromium --headless --no-sandbox --disable-gpu \
    --disable-dev-shm-usage --disable-features=WebRtcHideLocalIpsWithMdns \
    --renderer-process-limit=1 --remote-debugging-port=19222 \
    --remote-allow-origins='*' --user-data-dir="$work_dir/profile" about:blank >"$work_dir/chromium.log" 2>&1 &
browser_pid=$!
for attempt in {1..100}; do
    if curl -fsS http://127.0.0.1:19222/json >/dev/null 2>&1 && \
       curl -fsS http://127.0.0.1:49070/examples/echo/index.html >/dev/null 2>&1; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null || ! kill -0 "$browser_pid" 2>/dev/null; then
        echo 'test process failed to start' >&2; exit 1
    fi
    sleep 0.1
done
sleep 2
P2PDC_ECHO_ROLE="$native_role" P2PDC_ECHO_SEED="$native_seed" P2PDC_ECHO_SECRET="$native_secret" \
    P2PDC_ECHO_PEER_KEY="$native_key" P2PDC_ECHO_CAPABILITY="$capability" P2PDC_ECHO_NETWORK=873199 \
    P2PDC_ECHO_BOOTSTRAP_HOST=127.0.0.1 P2PDC_ECHO_BOOTSTRAP_PORT=49411 \
    setsid timeout 50 "$root/build-native/p2pdc_echo" >"$work_dir/peer.log" 2>&1 &
peer_pid=$!
python3 "$root/tests/browser_echo_standalone_cdp.py" \
    "http://127.0.0.1:49070/examples/echo/index.html" "$browser_key" "$browser_role" "$mode" "$work_dir/browser-result.log"
wait "$peer_pid"
peer_pid=
if [[ "$role" == browser-initiator && "$mode" == direct ]]; then
    python3 "$root/tests/browser_echo_key_cdp.py" \
        "http://127.0.0.1:49070/examples/echo/index.html" "$key_a"
fi
if [[ "$native_role" == initiator ]]; then
    grep -q '^echo received$' "$work_dir/peer.log"
else
    grep -q '^echo acknowledged; waiting for peer$' "$work_dir/peer.log"
fi
if [[ "$mode" == direct ]]; then
    if ! grep -q '^route: Direct' "$work_dir/peer.log" && \
       ! grep -q '^route: Direct' "$work_dir/browser-result.log"; then
        echo 'neither peer observed a direct route' >&2; exit 1
    fi
else
    if ! grep -q '^route: TurnRelayed (TURN used)' "$work_dir/peer.log" && \
       ! grep -q '^route: TurnRelayed (TURN used)' "$work_dir/browser-result.log"; then
        echo 'neither peer observed the TURN relay' >&2; exit 1
    fi
    nft -j list chain inet p2pdc_test output | python3 -c '
import json, sys
rules = json.load(sys.stdin)["nftables"]
packets = sum(expr["counter"]["packets"] for item in rules
              for expr in item.get("rule", {}).get("expr", []) if "counter" in expr)
if packets == 0:
    raise SystemExit("network filter did not block direct packets")
print(f"Isolated network filter blocked {packets} direct packets")'
fi
if grep -Eq '^route: Direct \[local=(PeerReflexive, remote=[A-Za-z]+|[A-Za-z]+, remote=PeerReflexive)\]' "$work_dir/peer.log"; then
    echo 'native peer-reflexive route was reported as direct' >&2; exit 1
fi
echo 'Standalone native and browser peers exchanged an echo and stopped'
INNER
