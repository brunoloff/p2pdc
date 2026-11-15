#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-android-adb-test.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
cat >"$test_dir/adb" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == -s && "$2" == test-device ]]
shift 2
if [[ "$1" == push ]]; then exit 0; fi
[[ "$1" == shell ]]
shift
case "$*" in
    'getprop ro.product.cpu.abilist') echo "${P2PDC_FAKE_ABI:-arm64-v8a}" ;;
    'getprop ro.build.version.sdk') echo 30 ;;
    'mktemp -d /data/local/tmp/p2pdc-test.XXXXXX') echo "${P2PDC_FAKE_DIR:-/data/local/tmp/p2pdc-test.ABC123}" ;;
    chmod*) ;;
    *external_consumer*) echo 'external consumer passed' ;;
    *native_lifecycle*)
        if [[ "${P2PDC_FAKE_LIFECYCLE_FAIL:-0}" == 1 ]]; then echo 'lifecycle failed' >&2; exit 1; fi
        echo 'native node lifecycle passed' ;;
    *native_pair*)
        if [[ "$*" == *--races* ]]; then echo races >>"$P2PDC_FAKE_LOG"; fi
        if [[ "${P2PDC_FAKE_PAIR_FAIL:-0}" == 1 ]]; then echo 'public pair failed'; exit 1; fi
        echo 'public node pair passed' ;;
    *native_smoke*direct)
        if [[ "${P2PDC_FAKE_RTC_FAIL:-0}" == 1 ]]; then echo 'binary_echo=0 route=Unknown'; exit 1; fi
        echo 'binary_echo=1 route=Direct'
        ;;
    *P2PDC_ECHO_SEED=*--public-key)
        if [[ "$*" == *"P2PDC_ECHO_SEED='00000000"* ]]; then
            if [[ "${P2PDC_FAKE_ACCEPT_ZERO:-0}" == 1 ]]; then echo invalid; exit 0; fi
            echo 'seed must be nonzero' >&2
            exit 2
        fi
        echo "${P2PDC_FAKE_KEY}"
        ;;
    'rm -rf '*)
        echo cleanup >>"$P2PDC_FAKE_LOG"
        if [[ "${P2PDC_FAKE_CLEANUP_FAIL:-0}" == 1 ]]; then exit 1; fi
        ;;
    *) echo "Unexpected fake ADB command: $*" >&2; exit 1 ;;
esac
FAKE
chmod +x "$test_dir/adb"
seed=$(printf '01%.0s' {1..32})
export P2PDC_FAKE_KEY
P2PDC_FAKE_KEY=$(P2PDC_ECHO_SEED="$seed" "$root/build-native/p2pdc_echo" --public-key)
export P2PDC_ADB="$test_dir/adb" P2PDC_FAKE_LOG="$test_dir/cleanup.log"
bash "$root/tests/run_android_device_smoke.sh" test-device
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 1 ]]
bash "$root/tests/run_android_device_smoke.sh" test-device --webrtc
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 2 ]]
bash "$root/tests/run_android_device_smoke.sh" test-device --pair
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 3 ]]
if P2PDC_FAKE_PAIR_FAIL=1 bash "$root/tests/run_android_device_smoke.sh" test-device --pair >"$test_dir/error" 2>&1; then
    echo 'A failed public pair smoke was not detected.' >&2; exit 1
fi
grep -q 'public node pair exited unsuccessfully' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 4 ]]
if P2PDC_FAKE_RTC_FAIL=1 bash "$root/tests/run_android_device_smoke.sh" test-device --webrtc >"$test_dir/error" 2>&1; then
    echo 'A failed WebRTC smoke was not detected.' >&2; exit 1
fi
grep -q 'WebRTC smoke exited unsuccessfully' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 5 ]]
if P2PDC_FAKE_LIFECYCLE_FAIL=1 bash "$root/tests/run_android_device_smoke.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'A failed lifecycle smoke was not detected.' >&2; exit 1
fi
grep -q 'lifecycle failed' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 6 ]]
if P2PDC_FAKE_ACCEPT_ZERO=1 bash "$root/tests/run_android_device_smoke.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'Invalid-seed acceptance was not detected.' >&2; exit 1
fi
grep -q 'accepted an invalid signing seed' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 7 ]]
if P2PDC_FAKE_KEY=wrong bash "$root/tests/run_android_device_smoke.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'A public-key mismatch was not detected.' >&2; exit 1
fi
grep -q 'public keys differ' "$test_dir/error"
if P2PDC_FAKE_CLEANUP_FAIL=1 bash "$root/tests/run_android_device_smoke.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'A device cleanup failure was not detected.' >&2; exit 1
fi
grep -q 'Could not remove device test directory' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 9 ]]
if P2PDC_FAKE_ABI=x86_64 bash "$root/tests/run_android_device_smoke.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'An incompatible device was not rejected.' >&2; exit 1
fi
grep -q 'requires an arm64' "$test_dir/error"
if P2PDC_FAKE_DIR=/data/local/tmp/other bash "$root/tests/run_android_device_smoke.sh" test-device >"$test_dir/error" 2>&1; then
    echo 'An unsafe test directory was not rejected.' >&2; exit 1
fi
grep -q 'safe temporary test directory' "$test_dir/error"
[[ $(wc -l <"$P2PDC_FAKE_LOG") == 9 ]]
bash "$root/tests/run_android_device_smoke.sh" test-device --races
[[ $(tail -n 2 "$P2PDC_FAKE_LOG") == $'races\ncleanup' ]]
if P2PDC_FAKE_PAIR_FAIL=1 bash "$root/tests/run_android_device_smoke.sh" test-device --races >"$test_dir/error" 2>&1; then
    echo 'A failed public race smoke was not detected.' >&2; exit 1
fi
grep -q 'public node races exited unsuccessfully' "$test_dir/error"
[[ $(tail -n 2 "$P2PDC_FAKE_LOG") == $'races\ncleanup' ]]
echo 'Android device smoke harness checks passed'
