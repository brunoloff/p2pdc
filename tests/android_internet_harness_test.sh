#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-internet-test.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
cat >"$test_dir/adb" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == -s && "$2" == test-device ]]
shift 2
case "$*" in
    'shell mktemp -d /data/local/tmp/p2pdc-test.XXXXXX') echo "${P2PDC_FAKE_DIR:-/data/local/tmp/p2pdc-test.ABC123}" ;;
    push*)
        if [[ "${P2PDC_FAKE_PUSH_OK:-0}" != 1 ]]; then echo 'simulated push failure' >&2; exit 1; fi ;;
    'shell chmod '*) ;;
    'shell echo '*)
        echo 'route: TurnRelayed (TURN used) [local=Relayed, remote=Relayed]'
        if [[ "${P2PDC_FAKE_STALE_ROUTE:-0}" == 1 ]]; then echo 'route: Unknown'; fi
        echo 'echo acknowledged; waiting for peer'
        if [[ "${P2PDC_FAKE_CLOSED_UNKNOWN:-0}" == 1 ]]; then echo 'route: Unknown (TURN used)'; fi ;;
    'shell if '*)
        echo cleanup >>"$P2PDC_FAKE_LOG"
        if [[ "${P2PDC_FAKE_CLEANUP_FAIL:-0}" == 1 ]]; then exit 1; fi ;;
    *) echo 'unexpected ADB command' >&2; exit 1 ;;
esac
FAKE
chmod +x "$test_dir/adb"
export P2PDC_ADB="$test_dir/adb" P2PDC_FAKE_LOG="$test_dir/cleanup.log"
if bash "$root/tests/run_android_internet_echo.sh" test-device invalid >"$test_dir/error" 2>&1; then
    echo 'Invalid role was accepted.' >&2; exit 1
fi
grep -q 'Usage:' "$test_dir/error"
if bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder phone-fallback >"$test_dir/error" 2>&1; then
    echo 'Fallback without isolation was accepted.' >&2; exit 1
fi
grep -q 'needs the isolated network runner' "$test_dir/error"
if bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder blocked-direct >"$test_dir/error" 2>&1; then
    echo 'Blocked-direct control without isolation was accepted.' >&2; exit 1
fi
grep -q 'needs the isolated network runner' "$test_dir/error"
if bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder relay >"$test_dir/error" 2>&1; then
    echo 'Missing relay configuration was accepted.' >&2; exit 1
fi
grep -q 'Relay test requires valid' "$test_dir/error"
if P2PDC_TEST_TURN_HOST='invalid;echo' P2PDC_TEST_TURN_USER=user P2PDC_TEST_TURN_PASSWORD=pass \
    bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder relay >"$test_dir/error" 2>&1; then
    echo 'Unsafe relay configuration was accepted.' >&2; exit 1
fi
grep -q 'Relay test requires valid' "$test_dir/error"
if P2PDC_FAKE_DIR=/data/local/tmp/other bash "$root/tests/run_android_internet_echo.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'Unsafe phone directory was accepted.' >&2; exit 1
fi
grep -q 'safe temporary test directory' "$test_dir/error"
[[ ! -e "$P2PDC_FAKE_LOG" ]]
if bash "$root/tests/run_android_internet_echo.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'Push failure was ignored.' >&2; exit 1
fi
grep -q 'simulated push failure' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 1 ]]
if P2PDC_FAKE_CLEANUP_FAIL=1 bash "$root/tests/run_android_internet_echo.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'Cleanup failure was ignored.' >&2; exit 1
fi
grep -q 'Could not clean phone test directory' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 2 ]]
cat >"$test_dir/runner" <<'FAKE'
#!/usr/bin/env bash
echo 'route: TurnRelayed (TURN used) [local=Relayed, remote=Relayed]'
echo 'echo received'
FAKE
chmod +x "$test_dir/runner"
export P2PDC_FAKE_PUSH_OK=1 P2PDC_TEST_NATIVE_RUNNER="$test_dir/runner"
export P2PDC_TEST_TURN_HOST=turn.example P2PDC_TEST_TURN_USER=test P2PDC_TEST_TURN_PASSWORD=test
bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder relay >"$test_dir/good" 2>&1
grep -q 'exchanged an internet echo' "$test_dir/good"
P2PDC_FAKE_CLOSED_UNKNOWN=1 bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder relay >"$test_dir/good" 2>&1
grep -q 'exchanged an internet echo' "$test_dir/good"
if P2PDC_FAKE_STALE_ROUTE=1 bash "$root/tests/run_android_internet_echo.sh" test-device phone-responder relay >"$test_dir/error" 2>&1; then
    echo 'A stale relay observation was accepted.' >&2; exit 1
fi
grep -q 'Local relay not observed by mobile at exchange' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 5 ]]
echo 'Android internet harness failure checks passed'
