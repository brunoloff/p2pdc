#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
"$root/build-external-native/external_consumer"
for command in unshare ip chromium curl python3 setsid; do command -v "$command" >/dev/null; done
python3 -c 'import websocket'
unshare --user --map-root-user --net --fork bash -s -- "$root" <<'INNER'
set -euo pipefail
root=$1
ip link set lo up
test_dir=$(mktemp -d /tmp/p2pdc-external-consumer.XXXXXX)
server_pid= browser_pid=
cleanup() {
    test_exit=$?
    for pid in "$browser_pid" "$server_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    if (( test_exit != 0 )); then
        for log in server chromium; do
            if [[ -f "$test_dir/$log.log" ]]; then tail -30 "$test_dir/$log.log" >&2; fi
        done
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
setsid python3 -m http.server 49070 --bind 127.0.0.1 --directory "$root" >"$test_dir/server.log" 2>&1 &
server_pid=$!
XDG_CONFIG_HOME="$test_dir/config" setsid chromium --headless --no-sandbox --disable-gpu \
    --disable-dev-shm-usage --renderer-process-limit=1 --remote-debugging-port=19222 \
    --remote-allow-origins='*' --user-data-dir="$test_dir/profile" about:blank >"$test_dir/chromium.log" 2>&1 &
browser_pid=$!
for attempt in {1..100}; do
    if curl -fsS http://127.0.0.1:19222/json >/dev/null 2>&1 && \
       curl -fsS http://127.0.0.1:49070/tests/external_consumer/index.html >/dev/null 2>&1; then break; fi
    if ! kill -0 "$server_pid" 2>/dev/null || ! kill -0 "$browser_pid" 2>/dev/null; then
        echo 'browser or web server failed to start' >&2; exit 1
    fi
    sleep 0.1
done
python3 "$root/tests/browser_cdp_smoke.py" http://127.0.0.1:49070/tests/external_consumer/index.html
INNER
