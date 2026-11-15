#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${P2PDC_NATIVE_TURN_TEST_BINARY:-"$root/build-native/p2pdc_native_turn_tests"}
if [[ ! -x "$binary" ]]; then echo 'Build p2pdc_native_turn_tests first.' >&2; exit 1; fi
for command in unshare ip turnserver setsid; do command -v "$command" >/dev/null; done
unshare --user --map-root-user --net --fork bash -s -- "$binary" <<'INNER'
set -euo pipefail
binary=$1
ip link set lo up
ip link add p2pa type veth peer name p2pb
ip addr add 10.44.0.1/24 dev p2pa
ip addr add 10.44.0.2/24 dev p2pb
ip link set p2pa up
ip link set p2pb up
ip route add default via 10.44.0.2 dev p2pa
test_dir=$(mktemp -d /tmp/p2pdc-native-public-turn.XXXXXX)
turn_pid=
cleanup() {
    test_exit=$?
    if [[ -n "$turn_pid" ]]; then kill -- "-$turn_pid" 2>/dev/null || true; fi
    wait 2>/dev/null || true
    if (( test_exit != 0 )) && [[ -f "$test_dir/turn.log" ]]; then tail -50 "$test_dir/turn.log" >&2; fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
setsid turnserver -c /dev/null --no-cli --no-tls --no-multicast-peers --relay-threads=0 \
    --listening-ip=10.44.0.1 --relay-ip=10.44.0.1 --listening-port=34789 \
    --min-port=10000 --max-port=10099 --lt-cred-mech --realm=p2pdc.test \
    --user=p2pdc:temporary-test-password --no-stdout-log --log-file="$test_dir/turn.log" \
    --pidfile="$test_dir/turn.pid" >"$test_dir/turn.out" 2>&1 &
turn_pid=$!
for attempt in {1..100}; do
    if (echo >/dev/tcp/10.44.0.1/34789) 2>/dev/null; then break; fi
    if ! kill -0 "$turn_pid" 2>/dev/null; then echo 'TURN server failed to start' >&2; exit 1; fi
    sleep 0.1
done
export P2PDC_TEST_TURN_HOST=10.44.0.1 P2PDC_TEST_TURN_USER=p2pdc
export P2PDC_TEST_TURN_PASSWORD=temporary-test-password
"$binary" --gtest_filter='NativePublicIncomingTest.BothPublicConnectionsReportForcedTurnRelay:NativePublicIncomingTest.AutomaticModeConnectsWhenPeerRequiresRelay:NativePublicIncomingTest.ForcedRelayRejectsBadCredentialsWithoutReadiness:NativeEchoTurnExampleTest.SharedExampleUsesAndReportsTurn'
INNER
