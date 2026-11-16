#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
binary=${P2PDC_SHARED_TEXT_TEST_BINARY:-"$root/build-native/examples/shared-text/p2pdc_shared_text_session_tests"}
if [[ ! -x "$binary" ]]; then
    echo "Build p2pdc_shared_text_session_tests and set P2PDC_SHARED_TEXT_TEST_BINARY if needed." >&2
    exit 1
fi
unshare --user --map-root-user --net --fork bash -s -- "$binary" <<'INNER'
set -euo pipefail
ip link set lo up
ip link add p2pa type veth peer name p2pb
ip addr add 10.42.0.1/24 dev p2pa
ip addr add 10.42.0.2/24 dev p2pb
ip link set p2pa up
ip link set p2pb up
ip route add default via 10.42.0.2 dev p2pa
exec "$1"
INNER
