#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary="$root/build-external-native/p2pdc_native_pair"
if [[ ! -x "$binary" ]]; then echo 'Build p2pdc_native_pair first.' >&2; exit 1; fi
test_dir=$(mktemp -d /tmp/p2pdc-native-pair.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
if "$binary" unexpected >"$test_dir/invalid.log" 2>&1; then
    echo 'Unexpected argument was accepted.' >&2; exit 1
else
    exit_code=$?
    if [[ "$exit_code" != 2 ]] || ! grep -q 'takes no arguments' "$test_dir/invalid.log"; then
        cat "$test_dir/invalid.log" >&2; exit 1
    fi
fi
unshare --user --map-root-user --net --fork bash -s -- "$binary" <<'INNER'
set -euo pipefail
ip link set lo up
ip link add p2pdc0 type veth peer name p2pdc1
ip addr add 10.42.0.1/24 dev p2pdc0
ip addr add 10.42.0.2/24 dev p2pdc1
ip link set p2pdc0 up
ip link set p2pdc1 up
timeout 40 "$1"
INNER
