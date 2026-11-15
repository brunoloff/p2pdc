#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
for command in unshare ip dhtnode setsid timeout; do command -v "$command" >/dev/null; done
unshare --user --map-root-user --net --fork bash -s -- "$root" <<'INNER'
set -euo pipefail
root=$1
unset P2PDC_ECHO_STUN_HOST P2PDC_ECHO_STUN_PORT P2PDC_ECHO_TURN_HOST P2PDC_ECHO_TURN_PORT
unset P2PDC_ECHO_TURN_USER P2PDC_ECHO_TURN_PASSWORD P2PDC_ECHO_TURN_TRANSPORT P2PDC_ECHO_RELAY_POLICY
unset P2PDC_ECHO_PROXY
ip link set lo up
ip link add p2pdc-test0 type veth peer name p2pdc-test1
ip addr add 10.42.0.1/24 dev p2pdc-test0
ip addr add 10.42.0.2/24 dev p2pdc-test1
ip link set p2pdc-test0 up
ip link set p2pdc-test1 up
ip route add default via 10.42.0.2 dev p2pdc-test0
work_dir=$(mktemp -d /tmp/p2pdc-standalone.XXXXXX)
dht_pid= extra_pid= initiator_pid= responder_pid=
cleanup() {
    test_exit=$?
    for pid in "$initiator_pid" "$responder_pid" "$extra_pid" "$dht_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    if (( test_exit != 0 )); then
        for log in initiator responder dht extra; do
            if [[ -f "$work_dir/$log.log" ]]; then tail -50 "$work_dir/$log.log" >&2; fi
        done
    fi
    rm -rf -- "$work_dir"
}
trap cleanup EXIT
set +e
env -i PATH="$PATH" "$root/build-native/p2pdc_echo" >"$work_dir/invalid.log" 2>&1
invalid_status=$?
P2PDC_ECHO_SEED=$(printf '00%.0s' {1..32}) "$root/build-native/p2pdc_echo" --public-key \
    >"$work_dir/invalid-key.log" 2>&1
invalid_key_status=$?
set -e
if [[ "$invalid_status" != 2 || "$invalid_key_status" != 2 ]] || \
   ! grep -q 'echo configuration:' "$work_dir/invalid.log" || \
   ! grep -q 'must be nonzero' "$work_dir/invalid-key.log"; then
    echo 'invalid configuration was not rejected as expected' >&2
    exit 1
fi
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49411' >"$work_dir/dht.log" 2>&1 &
dht_pid=$!
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49413 -b 127.0.0.1:49411' >"$work_dir/extra.log" 2>&1 &
extra_pid=$!
sleep 1
seed_a=$(printf '01%.0s' {1..32})
seed_b=$(printf '02%.0s' {1..32})
secret_a=$(printf '03%.0s' {1..32})
secret_b=$(printf '04%.0s' {1..32})
capability=$(printf '05%.0s' {1..32})
key_a=$(P2PDC_ECHO_SEED="$seed_a" "$root/build-native/p2pdc_echo" --public-key)
key_b=$(P2PDC_ECHO_SEED="$seed_b" "$root/build-native/p2pdc_echo" --public-key)
export P2PDC_ECHO_CAPABILITY="$capability" P2PDC_ECHO_NETWORK=873199
export P2PDC_ECHO_BOOTSTRAP_HOST=127.0.0.1 P2PDC_ECHO_BOOTSTRAP_PORT=49411
P2PDC_ECHO_ROLE=responder P2PDC_ECHO_SEED="$seed_b" P2PDC_ECHO_SECRET="$secret_b" \
    P2PDC_ECHO_PEER_KEY="$key_a" setsid timeout 45 "$root/build-native/p2pdc_echo" >"$work_dir/responder.log" 2>&1 &
responder_pid=$!
P2PDC_ECHO_ROLE=initiator P2PDC_ECHO_SEED="$seed_a" P2PDC_ECHO_SECRET="$secret_a" \
    P2PDC_ECHO_PEER_KEY="$key_b" setsid timeout 45 "$root/build-native/p2pdc_echo" >"$work_dir/initiator.log" 2>&1 &
initiator_pid=$!
wait "$initiator_pid"
initiator_pid=
wait "$responder_pid"
responder_pid=
grep -q '^echo received$' "$work_dir/initiator.log"
grep -q '^echo acknowledged; waiting for peer$' "$work_dir/responder.log"
grep -q '^route: Direct' "$work_dir/initiator.log"
grep -q '^route: Direct' "$work_dir/responder.log"
echo 'Standalone native peers exchanged an echo and reported Direct'
INNER
