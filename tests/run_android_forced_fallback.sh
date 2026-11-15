#!/usr/bin/env bash
set -euo pipefail
set +x
umask 077
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 1 || $# -gt 2 || -z "$1" || ( ${2:-phone-initiator} != phone-initiator && ${2:-phone-initiator} != phone-responder ) ]]; then
    echo 'Usage: run_android_forced_fallback.sh DEVICE_SERIAL [phone-initiator|phone-responder]' >&2
    exit 2
fi
for command in unshare nsenter slirp4netns nft getent python3 setsid; do command -v "$command" >/dev/null; done
if [[ -n ${P2PDC_TEST_BROWSER_RUNNER:-} && ! -x "$P2PDC_TEST_BROWSER_RUNNER" ]]; then
    echo 'Browser test runner is not executable.' >&2; exit 2
fi
if [[ ! -c /dev/net/tun ]]; then echo 'A usable /dev/net/tun is required for isolated networking.' >&2; exit 1; fi
test_dir=$(mktemp -d /tmp/p2pdc-fallback-net.XXXXXX)
ns_pid='' slirp_pid=''
cleanup() {
    exit_code=$?
    trap - EXIT
    if [[ -n "$slirp_pid" ]]; then kill "$slirp_pid" 2>/dev/null || true; wait "$slirp_pid" 2>/dev/null || true; fi
    if [[ -n "$ns_pid" ]]; then kill -- "-$ns_pid" 2>/dev/null || true; wait "$ns_pid" 2>/dev/null || true; fi
    rm -rf -- "$test_dir"
    exit "$exit_code"
}
trap cleanup EXIT
services=()
for host in "${P2PDC_TEST_TURN_HOST:?Set P2PDC_TEST_TURN_HOST}" stun.l.google.com; do
    previous_count=${#services[@]}
    while read -r address _; do
        if [[ "$address" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then services+=("$address"); fi
    done < <(getent ahostsv4 "$host")
    if (( ${#services[@]} == previous_count )); then echo 'Could not resolve a test service.' >&2; exit 1; fi
done
printf -v service_elements '%s, ' "${services[@]}"
service_elements=${service_elements%, }
cat >"$test_dir/filter.nft" <<EOF
table inet p2pdc_filter {
  set services { type ipv4_addr; elements = { $service_elements } }
  chain input { type filter hook input priority 0; policy accept;
    ip saddr @services meta l4proto udp accept
    meta l4proto udp @th,64,16 { 0x0001, 0x0101, 0x0111 } @th,96,32 0x2112a442 counter drop
  }
  chain output { type filter hook output priority 0; policy accept;
    ip daddr @services meta l4proto udp accept
    meta l4proto udp @th,64,16 { 0x0001, 0x0101, 0x0111 } @th,96,32 0x2112a442 counter drop
  }
}
EOF
mkfifo "$test_dir/control" "$test_dir/ready"
exec 3<>"$test_dir/control" 4<>"$test_dir/ready"
# The child shell expands its own PID and argument inside the namespace.
# shellcheck disable=SC2016
setsid unshare --user --map-root-user --net --fork sh -c 'echo $$ > "$1/pid"; IFS= read -r ignored < "$1/control"' _ "$test_dir" &
ns_pid=$!
for _ in {1..100}; do [[ -s "$test_dir/pid" ]] && break; sleep 0.05; done
if [[ ! -s "$test_dir/pid" ]]; then echo 'Network namespace failed to start.' >&2; exit 1; fi
child_pid=$(cat "$test_dir/pid")
slirp4netns --configure --disable-host-loopback --ready-fd=4 "$child_pid" tap0 4>&4 >"$test_dir/slirp.log" 2>&1 &
slirp_pid=$!
if ! read -r -t 10 -n 1 -u 4; then cat "$test_dir/slirp.log" >&2; echo 'Isolated network failed to start.' >&2; exit 1; fi
nsenter --target "$child_pid" --user --net --preserve-credentials nft -f "$test_dir/filter.nft"
if [[ -n ${P2PDC_TEST_BROWSER_RUNNER:-} ]]; then
    ln -s -- "$P2PDC_TEST_BROWSER_RUNNER" "$test_dir/browser-runner"
    printf '#!/bin/sh\nexec nsenter --target %s --user --net --preserve-credentials "%s/browser-runner" "$@"\n' \
        "$child_pid" "$test_dir" >"$test_dir/native-runner"
else
    printf '#!/bin/sh\nexec nsenter --target %s --user --net --preserve-credentials "$@"\n' "$child_pid" >"$test_dir/native-runner"
fi
chmod 700 "$test_dir/native-runner"
P2PDC_TEST_NATIVE_RUNNER="$test_dir/native-runner" bash "$root/tests/run_android_internet_echo.sh" "$1" "${2:-phone-initiator}" phone-fallback
nsenter --target "$child_pid" --user --net --preserve-credentials nft -j list table inet p2pdc_filter >"$test_dir/relay-counters.json"
python3 - "$test_dir/relay-counters.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    rules = json.load(source)["nftables"]
packets = sum(expr["counter"]["packets"] for item in rules
              for expr in item.get("rule", {}).get("expr", []) if "counter" in expr)
if packets == 0:
    raise SystemExit("isolated filter did not block direct ICE checks")
print(f"Isolated filter blocked {packets} direct ICE checks")
PY
if P2PDC_TEST_NATIVE_RUNNER="$test_dir/native-runner" bash "$root/tests/run_android_internet_echo.sh" \
    "$1" "${2:-phone-initiator}" blocked-direct >"$test_dir/control.log" 2>&1; then
    echo 'Direct connection unexpectedly succeeded without TURN.' >&2; exit 1
fi
if ! grep -Eq 'echo: connection failed \(code (4|6)\)' "$test_dir/control.log"; then
    cat "$test_dir/control.log" >&2
    echo 'No-TURN control did not reach an ICE connection failure.' >&2; exit 1
fi
nsenter --target "$child_pid" --user --net --preserve-credentials nft -j list table inet p2pdc_filter >"$test_dir/control-counters.json"
if ! python3 - "$test_dir/relay-counters.json" "$test_dir/control-counters.json" <<'PY'
import json, sys
def count(path):
    with open(path, encoding="utf-8") as source:
        rules = json.load(source)["nftables"]
    return sum(expr["counter"]["packets"] for item in rules
               for expr in item.get("rule", {}).get("expr", []) if "counter" in expr)
if count(sys.argv[2]) <= count(sys.argv[1]):
    raise SystemExit("no-TURN control did not block a direct ICE check")
print("No-TURN control blocked direct ICE checks and the connection failed")
PY
then
    cat "$test_dir/control.log" >&2
    exit 1
fi
