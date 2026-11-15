#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-direct}
browser=${P2PDC_TEST_BROWSER:-chromium}
if [[ $# -gt 1 || ( "$mode" != direct && "$mode" != unsupported && "$mode" != broken-transport && "$mode" != accept-unsupported && "$mode" != accept-broken-transport && "$mode" != turn && "$mode" != fallback && "$mode" != echo && "$mode" != echo-reject && "$mode" != echo-turn && "$mode" != echo-fallback && "$mode" != collision && "$mode" != cancel && "$mode" != reject && "$mode" != cancel-accepted && "$mode" != tls && "$mode" != tls-untrusted && "$mode" != tls-name && "$mode" != tls-timed && "$mode" != tls-expired ) ]]; then
    printf 'Usage: %s [direct|unsupported|broken-transport|accept-unsupported|accept-broken-transport|turn|fallback|echo|echo-reject|echo-turn|echo-fallback|collision|cancel|reject|cancel-accepted|tls|tls-untrusted|tls-name|tls-timed|tls-expired]\n' "$0" >&2
    exit 1
fi
if [[ "$browser" != chromium && "$browser" != firefox && "$browser" != webkit ]]; then
    echo 'P2PDC_TEST_BROWSER must be chromium, firefox or webkit' >&2; exit 1
fi
tls_mode=0
if [[ "$mode" == tls* ]]; then tls_mode=1; fi
if [[ "$tls_mode" == 1 && "$browser" == webkit ]]; then
    echo 'TLS certificate tests currently require Firefox or Chromium.' >&2; exit 1
fi
for command in unshare ip dhtnode curl python3 setsid; do command -v "$command" >/dev/null; done
if [[ "$browser" == chromium ]]; then command -v chromium >/dev/null; python3 -c 'import websocket'; fi
if [[ "$browser" == firefox || "$tls_mode" == 1 ]]; then python3 -c 'import playwright.sync_api'; fi
if [[ "$browser" == webkit ]]; then command -v xvfb-run >/dev/null; fi
if [[ "$mode" == turn || "$mode" == fallback || "$mode" == echo-turn || "$mode" == echo-fallback || "$tls_mode" == 1 ]]; then command -v nft >/dev/null; command -v turnserver >/dev/null; fi
if [[ "$tls_mode" == 1 ]]; then command -v openssl >/dev/null; command -v certutil >/dev/null; fi
namespace_options=(--user --map-root-user --net --fork)
if [[ "$tls_mode" == 1 && "$browser" == chromium ]]; then namespace_options+=(--mount); fi
unshare "${namespace_options[@]}" bash -s -- "$root" "$mode" "$browser" "$tls_mode" <<'INNER'
set -euo pipefail
root=$1
mode=$2
browser=$3
tls_mode=$4
ip link set lo up
ip link add p2pdc-test0 type veth peer name p2pdc-test1
ip addr add 10.42.0.1/24 dev p2pdc-test0
ip addr add 10.42.0.2/24 dev p2pdc-test1
ip link set p2pdc-test0 up
ip link set p2pdc-test1 up
ip route add default via 10.42.0.2 dev p2pdc-test0
if [[ "$mode" == turn || "$mode" == fallback || "$mode" == echo-turn || "$mode" == echo-fallback || "$tls_mode" == 1 ]]; then
    nft add table inet p2pdc_test
    nft 'add chain inet p2pdc_test output { type filter hook output priority 0; policy accept; }'
    nft add rule inet p2pdc_test output ip saddr 127.0.0.0/8 ip daddr 127.0.0.0/8 accept
    nft add rule inet p2pdc_test output udp sport != 10000-10099 udp dport != 10000-10099 counter drop
    nft add rule inet p2pdc_test output tcp sport != 34789 tcp dport != 34789 tcp sport != 53490 tcp dport != 53490 counter drop
fi
test_dir=$(mktemp -d /tmp/p2pdc-public-connection.XXXXXX)
hidden_pki=0
if [[ "$tls_mode" == 1 && "$browser" == chromium && -d "$HOME/.pki/nssdb" ]]; then
    mount --make-rprivate /
    mkdir "$test_dir/hidden-pki"
    mount --bind "$test_dir/hidden-pki" "$HOME/.pki"
    hidden_pki=1
fi
dht_pid= extra_pid= peer_pid= server_pid= browser_pid= turn_pid=
cleanup() {
    test_exit=$?
    for pid in "$browser_pid" "$server_pid" "$peer_pid" "$extra_pid" "$dht_pid" "$turn_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    if [[ "$hidden_pki" == 1 ]]; then umount "$HOME/.pki"; fi
    if (( test_exit != 0 )); then
        for log in dht peer server chromium turn; do
            if [[ -f "$test_dir/$log.log" ]]; then tail -60 "$test_dir/$log.log" >&2; fi
        done
        if [[ -f "$test_dir/turn.out" ]]; then tail -30 "$test_dir/turn.out" >&2; fi
        for logfile in "$test_dir"/turn*.log; do
            if [[ -f "$logfile" ]]; then tail -30 "$logfile" >&2; fi
        done
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
if [[ "$tls_mode" == 1 ]]; then
    mkdir "$test_dir/firefox-profile"
    cert_db="$test_dir/firefox-profile"
    if [[ "$browser" == chromium ]]; then
        mkdir -p "$test_dir/chromium-data/pki/nssdb"
        cert_db="$test_dir/chromium-data/pki/nssdb"
    fi
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj '/CN=p2pdc test CA' \
        -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' \
        -keyout "$test_dir/ca.key" -out "$test_dir/ca.crt" >"$test_dir/cert.log" 2>&1
    cert_name=turn.test
    if [[ "$mode" == tls-name ]]; then cert_name=wrong.test; fi
    openssl req -new -newkey rsa:2048 -nodes -subj "/CN=$cert_name" \
        -keyout "$test_dir/server.key" -out "$test_dir/server.csr" >>"$test_dir/cert.log" 2>&1
    printf '%s\n' "subjectAltName=DNS:$cert_name" 'basicConstraints=critical,CA:FALSE' \
        'keyUsage=critical,digitalSignature,keyEncipherment' 'extendedKeyUsage=serverAuth' >"$test_dir/extensions"
    openssl x509 -req -in "$test_dir/server.csr" -CA "$test_dir/ca.crt" -CAkey "$test_dir/ca.key" \
        -CAcreateserial -days 1 -extfile "$test_dir/extensions" -out "$test_dir/server.crt" >>"$test_dir/cert.log" 2>&1
    certutil -N -d "sql:$cert_db" --empty-password
    if [[ "$mode" != tls-untrusted ]]; then
        certutil -A -d "sql:$cert_db" -n p2pdc-test-ca -t 'C,,' -i "$test_dir/ca.crt"
    fi
    turn_auth=(--lt-cred-mech --user=p2pdc:temporary-test-password)
    if [[ "$mode" == tls-timed || "$mode" == tls-expired ]]; then
        turn_auth=(--use-auth-secret --static-auth-secret=p2pdc-test-secret)
    fi
    setsid turnserver -c /dev/null --verbose --simple-log --no-cli --no-multicast-peers --relay-threads=0 \
        --listening-ip=127.0.0.1 --listening-ip=10.42.0.1 --relay-ip=10.42.0.1 --listening-port=34789 \
        --tls-listening-port=53490 --cert="$test_dir/server.crt" --pkey="$test_dir/server.key" \
        --min-port=10000 --max-port=10099 --realm=p2pdc.test \
        "${turn_auth[@]}" --no-stdout-log --log-file="$test_dir/turn.log" \
        --pidfile="$test_dir/turn.pid" >"$test_dir/turn.out" 2>&1 &
    turn_pid=$!
    for attempt in {1..100}; do
        if (echo >/dev/tcp/127.0.0.1/53490) 2>/dev/null; then break; fi
        if ! kill -0 "$turn_pid" 2>/dev/null; then echo 'TLS TURN server failed to start' >&2; exit 1; fi
        sleep 0.1
    done
    timeout 5 openssl s_client -connect 127.0.0.1:53490 -CAfile "$test_dir/ca.crt" \
        -servername turn.test -verify_return_error -verify_hostname "$cert_name" </dev/null >"$test_dir/tls-check.log" 2>&1
elif [[ "$mode" == turn || "$mode" == fallback || "$mode" == echo-turn || "$mode" == echo-fallback ]]; then
    setsid turnserver -c /dev/null --no-cli --no-tls --no-multicast-peers --relay-threads=0 \
        --listening-ip=10.42.0.1 --relay-ip=10.42.0.1 --listening-port=34789 \
        --min-port=10000 --max-port=10099 --lt-cred-mech --realm=p2pdc.test \
        --user=p2pdc:temporary-test-password --no-stdout-log --log-file="$test_dir/turn.log" \
        --pidfile="$test_dir/turn.pid" >"$test_dir/turn.out" 2>&1 &
    turn_pid=$!
    for attempt in {1..100}; do
        if (echo >/dev/tcp/10.42.0.1/34789) 2>/dev/null; then break; fi
        if ! kill -0 "$turn_pid" 2>/dev/null; then echo 'TURN server failed to start' >&2; exit 1; fi
        sleep 0.1
    done
fi
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49411 --proxyserver 49412' >"$test_dir/dht.log" 2>&1 &
dht_pid=$!
for attempt in {1..100}; do
    if curl -sS -o /dev/null http://127.0.0.1:49412/ 2>/dev/null; then break; fi
    if ! kill -0 "$dht_pid" 2>/dev/null; then echo 'DHT proxy failed to start' >&2; exit 1; fi
    sleep 0.1
done
setsid bash -c 'tail -f /dev/null | dhtnode -n 873199 -p 49413 -b 127.0.0.1:49411' >"$test_dir/extra.log" 2>&1 &
extra_pid=$!
setsid "$root/build-native/p2pdc_browser_record_native_peer" >"$test_dir/peer.log" 2>&1 &
peer_pid=$!
for attempt in {1..150}; do
    if grep -q '^READY$' "$test_dir/peer.log"; then break; fi
    if ! kill -0 "$peer_pid" 2>/dev/null; then echo 'native peer failed to start' >&2; exit 1; fi
    sleep 0.1
done
if ! grep -q '^READY$' "$test_dir/peer.log"; then echo 'native peer startup timed out' >&2; exit 1; fi
setsid python3 -m http.server 49070 --bind 127.0.0.1 --directory "$root" >"$test_dir/server.log" 2>&1 &
server_pid=$!
if [[ "$browser" == chromium && "$tls_mode" == 0 ]]; then
    XDG_CONFIG_HOME="$test_dir/config" setsid chromium --headless --no-sandbox --disable-gpu \
    --disable-dev-shm-usage --disable-features=WebRtcHideLocalIpsWithMdns \
    --renderer-process-limit=1 --remote-debugging-port=19222 \
    --remote-allow-origins='*' --user-data-dir="$test_dir/profile" about:blank >"$test_dir/chromium.log" 2>&1 &
    browser_pid=$!
fi
for attempt in {1..100}; do
    if curl -fsS http://127.0.0.1:49070/tests/browser_public_connection_smoke.html >/dev/null 2>&1 && \
       { [[ "$browser" != chromium || "$tls_mode" == 1 ]] || curl -fsS http://127.0.0.1:19222/json >/dev/null 2>&1; }; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null || { [[ -n "$browser_pid" ]] && ! kill -0 "$browser_pid" 2>/dev/null; }; then
        echo 'browser or web server failed to start' >&2; exit 1
    fi
    sleep 0.1
done
# Chromium can emit its initial network-change notification shortly after the
# debugging endpoint opens, aborting unrelated fetches in the new namespace.
sleep 2
page=http://127.0.0.1:49070/tests/browser_public_connection_smoke.html
if [[ "$mode" == unsupported ]]; then page="$page?unsupported=1"; fi
if [[ "$mode" == broken-transport ]]; then page="$page?unsupported=broken"; fi
if [[ "$mode" == accept-unsupported ]]; then page="$page?accept-unsupported=1"; fi
if [[ "$mode" == accept-broken-transport ]]; then page="$page?accept-unsupported=broken"; fi
if [[ "$mode" == turn ]]; then page="$page?turn=required"; fi
if [[ "$mode" == fallback ]]; then page="$page?turn=fallback"; fi
if [[ "$mode" == echo ]]; then page="$page?example=echo"; fi
if [[ "$mode" == echo-reject ]]; then page="$page?example=reject"; fi
if [[ "$mode" == echo-turn ]]; then page="$page?example=echo&turn=required"; fi
if [[ "$mode" == echo-fallback ]]; then page="$page?example=echo&turn=fallback"; fi
if [[ "$mode" == tls || "$mode" == tls-untrusted || "$mode" == tls-name ]]; then page="$page?turn=tls"; fi
if [[ "$mode" == tls-timed || "$mode" == tls-expired ]]; then page="$page?turn=$mode"; fi
if [[ "$mode" == collision || "$mode" == cancel || "$mode" == reject || "$mode" == cancel-accepted ]]; then page="$page?race=$mode"; fi
if [[ "$tls_mode" == 1 ]]; then
    args=(--timeout 35 --profile "$test_dir/firefox-profile" --engine "$browser")
    if [[ "$mode" == tls-untrusted || "$mode" == tls-name || "$mode" == tls-expired ]]; then args+=(--expect-failure); fi
    if [[ "$mode" == tls-expired ]]; then args+=(--expect-ice-error 401); fi
    XDG_DATA_HOME="$test_dir/chromium-data" python3 "$root/tests/browser_playwright_smoke.py" "${args[@]}" "$page"
    python3 - "$test_dir/turn.log" "$mode" <<'PY'
import re
import sys
from pathlib import Path

log = Path(sys.argv[1]).read_text()
allocations = re.findall(r"allocation new,.*username=<[^>]*p2pdc>.*method=TLSv1\.[23]", log)
if sys.argv[2] in ("tls", "tls-timed") and len(allocations) < 2:
    raise SystemExit("TURN server did not report two authenticated TLS allocations")
if sys.argv[2] in ("tls-untrusted", "tls-name", "tls-expired") and allocations:
    raise SystemExit("negative control unexpectedly produced a TLS allocation")
if sys.argv[2] == "tls-expired" and "credentials of user <1:p2pdc> have expired" not in log:
    raise SystemExit("TURN server did not report credential expiry")
print(f"TURN server reported {len(allocations)} authenticated TLS allocations")
PY
elif [[ "$browser" == firefox ]]; then
    python3 "$root/tests/browser_playwright_smoke.py" --timeout 35 "$page"
elif [[ "$browser" == webkit ]]; then
    GSETTINGS_BACKEND=memory XDG_CACHE_HOME="$test_dir/cache" XDG_CONFIG_HOME="$test_dir/config" \
        xvfb-run -a python3 "$root/tests/browser_webkit_smoke.py" --timeout 35 "$page"
else
    python3 "$root/tests/browser_cdp_smoke.py" --timeout 35 "$page"
fi
if [[ "$mode" == turn || "$mode" == fallback || "$mode" == echo-turn || "$mode" == echo-fallback ]]; then
    nft -j list chain inet p2pdc_test output | python3 -c '
import json, sys
rules = json.load(sys.stdin)["nftables"]
packets = sum(expr["counter"]["packets"] for item in rules
              for expr in item.get("rule", {}).get("expr", []) if "counter" in expr)
if packets == 0:
    raise SystemExit("network filter did not block any direct packets")
print(f"Isolated network filter blocked {packets} direct packets")'
fi
INNER
