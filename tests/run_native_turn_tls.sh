#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mode=${1:-trusted}
if [[ $# -gt 1 ]]; then echo 'Usage: run_native_turn_tls.sh MODE' >&2; exit 2; fi
case "$mode" in unit|trusted|fallback|untrusted|hostname|wrong-dns|trusted-ip|expired|plaintext|alternate|unreachable) ;; *) exit 2 ;; esac
for command in unshare ip openssl python3 setsid; do command -v "$command" >/dev/null; done
if [[ "$mode" != unit && "$mode" != plaintext && "$mode" != unreachable ]]; then command -v turnserver >/dev/null; fi
if [[ "$mode" == unit && ! -x "$root/.deps/native_tls_socket_unit" ]]; then
    echo 'Build the TLS socket unit test first.' >&2; exit 1
fi
if [[ "$mode" != unit && ! -x "$root/.deps/native_turn_tls_probe" ]]; then
    echo 'Build the TLS probe first.' >&2; exit 1
fi
if [[ "$mode" == fallback ]]; then
    command -v nft >/dev/null
    if [[ ! -x ${P2PDC_TLS_TEST_BINARY:-} ]]; then
        echo 'Set P2PDC_TLS_TEST_BINARY to the public TURN test binary.' >&2; exit 1
    fi
fi
unshare --user --map-root-user --net --fork bash -s -- "$root" "$mode" <<'INNER'
set -euo pipefail
root=$1 mode=$2
ip link set lo up
ip link add tls-test0 type veth peer name tls-test1
ip addr add 10.42.0.1/24 dev tls-test0
ip addr add 10.42.0.2/24 dev tls-test1
ip link set tls-test0 up
ip link set tls-test1 up
if [[ "$mode" == fallback ]]; then
    ip route add default via 10.42.0.2 dev tls-test0
    nft add table inet p2pdc_tls_test
    nft 'add chain inet p2pdc_tls_test output { type filter hook output priority 0; policy accept; }'
    nft add rule inet p2pdc_tls_test output ip saddr 127.0.0.0/8 ip daddr 127.0.0.0/8 accept
    nft add rule inet p2pdc_tls_test output udp sport != 10000-10099 udp dport != 10000-10099 counter drop
    nft add rule inet p2pdc_tls_test output tcp sport != 53490 tcp dport != 53490 counter drop
fi
dir=$(mktemp -d /tmp/p2pdc-tls-probe.XXXXXX)
server=
cleanup() {
    status=$?
    if [[ -n "$server" ]]; then kill -- "-$server" 2>/dev/null || true; fi
    wait 2>/dev/null || true
    if (( status != 0 )); then
        for log in "$dir"/turn.log "$dir"/plain.log; do [[ ! -f "$log" ]] || tail -30 "$log" >&2; done
        if [[ -f "$dir/client.log" ]]; then grep -Ei 'alternate|discovery|failed|error|300' "$dir/client.log" | tail -60 >&2 || true; fi
    fi
    rm -rf -- "$dir"
}
trap cleanup EXIT
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj '/CN=p2pdc probe CA' \
    -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' \
    -keyout "$dir/ca.key" -out "$dir/ca.crt" >"$dir/cert.log" 2>&1
openssl req -new -newkey rsa:2048 -nodes -subj '/CN=localhost' \
    -keyout "$dir/server.key" -out "$dir/server.csr" >>"$dir/cert.log" 2>&1
cert_identity=DNS:localhost
if [[ "$mode" == wrong-dns ]]; then cert_identity=DNS:wrong.test; fi
if [[ "$mode" == trusted-ip ]]; then cert_identity=IP:127.0.0.1; fi
printf '%s\n' "subjectAltName=$cert_identity" 'basicConstraints=critical,CA:FALSE' \
    'keyUsage=critical,digitalSignature,keyEncipherment' 'extendedKeyUsage=serverAuth' >"$dir/ext"
openssl x509 -req -in "$dir/server.csr" -CA "$dir/ca.crt" -CAkey "$dir/ca.key" \
    -CAcreateserial -days 1 -extfile "$dir/ext" -out "$dir/server.crt" >>"$dir/cert.log" 2>&1
if [[ "$mode" == unit ]]; then
    "$root/.deps/native_tls_socket_unit" "$dir/server.crt" "$dir/server.key"
    echo 'native TLS socket unit test passed'
    exit 0
fi
if [[ "$mode" == unreachable ]]; then
    :
elif [[ "$mode" == plaintext ]]; then
    setsid python3 -c 'import socket,sys; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",53490)); s.listen(5); c,_=s.accept(); data=c.recv(4096); open(sys.argv[1],"wb").write(data); c.sendall(b"not TLS"); c.close(); s.close()' "$dir/first-record" >"$dir/plain.log" 2>&1 &
else
    auth=(--lt-cred-mech --user=p2pdc:temporary-test-password)
    if [[ "$mode" == expired ]]; then auth=(--use-auth-secret --static-auth-secret=p2pdc-test-secret); fi
    alternate=()
    if [[ "$mode" == alternate ]]; then alternate=(--tls-alternate-server=127.0.0.1:53491); fi
    setsid turnserver -c /dev/null --verbose --simple-log --no-cli --no-multicast-peers --relay-threads=0 \
        --listening-ip=127.0.0.1 --relay-ip=10.42.0.1 --listening-port=34789 \
        --tls-listening-port=53490 --cert="$dir/server.crt" --pkey="$dir/server.key" \
        --min-port=10000 --max-port=10099 --realm=p2pdc.test "${auth[@]}" "${alternate[@]}" \
        --no-stdout-log --log-file="$dir/turn.log" \
        --pidfile="$dir/turn.pid" >"$dir/turn.out" 2>&1 &
fi
if [[ "$mode" != unreachable ]]; then server=$!; fi
sleep 1
host=localhost expected=expect-relay
if [[ "$mode" == hostname || "$mode" == trusted-ip ]]; then host=127.0.0.1; fi
if [[ "$mode" != trusted && "$mode" != trusted-ip && "$mode" != fallback ]]; then expected=expect-none; fi
if [[ "$mode" == untrusted ]]; then export SSL_CERT_FILE=/dev/null; else export SSL_CERT_FILE="$dir/ca.crt"; fi
export P2PDC_TEST_TURN_HOST="$host" P2PDC_TEST_TURN_USER=p2pdc P2PDC_TEST_TURN_PASSWORD=temporary-test-password
if [[ "$mode" == expired ]]; then
    export P2PDC_TEST_TURN_USER=1:p2pdc P2PDC_TEST_TURN_PASSWORD='8zsChuzHi08+WyOX+JXi2hVp9EA='
fi
if [[ "$mode" == alternate ]]; then
    NICE_DEBUG=all G_MESSAGES_DEBUG=all "$root/.deps/native_turn_tls_probe" "$host" "$expected" >"$dir/client.log" 2>&1
    grep -q 'TURN alternate server socket creation failed' "$dir/client.log"
fi
if [[ -n ${P2PDC_TLS_TEST_BINARY:-} ]]; then
    test_case=NativePublicIncomingTest.TlsRelayFailureDoesNotBecomeReady
    if [[ "$expected" == expect-relay ]]; then test_case=NativePublicIncomingTest.TlsRelayConnectsAndExchangesAuthenticatedMessages; fi
    if [[ "$mode" == fallback ]]; then test_case=NativePublicIncomingTest.TlsAutomaticFallbackReportsRelay; fi
    "$P2PDC_TLS_TEST_BINARY" --gtest_filter="$test_case"
elif [[ "$mode" != alternate ]]; then
    "$root/.deps/native_turn_tls_probe" "$host" "$expected"
fi
if [[ "$expected" == expect-relay ]]; then
    grep -q 'TLS.*session' "$dir/turn.log"
elif [[ "$mode" == plaintext ]]; then
    python3 -c 'import pathlib,sys; data=pathlib.Path(sys.argv[1]).read_bytes(); assert len(data)>5 and data[:2]==b"\x16\x03", data[:12]' "$dir/first-record"
elif [[ "$mode" != unreachable ]]; then
    grep -qi 'session' "$dir/turn.log"
    if [[ "$mode" == expired ]]; then grep -qi 'expired' "$dir/turn.log"; fi
    if [[ "$mode" == alternate ]]; then grep -qi 'alternate' "$dir/turn.log"; fi
fi
if [[ "$mode" == fallback ]]; then
    nft -j list chain inet p2pdc_tls_test output | python3 -c '
import json, sys
rules = json.load(sys.stdin)["nftables"]
packets = sum(expr["counter"]["packets"] for item in rules
              for expr in item.get("rule", {}).get("expr", []) if "counter" in expr)
if not packets:
    raise SystemExit("network filter did not block direct packets")
print(f"Isolated network filter blocked {packets} direct packets")'
fi
echo "native TLS probe passed: $mode"
INNER
