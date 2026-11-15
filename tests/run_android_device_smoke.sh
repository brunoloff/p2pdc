#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 1 || $# -gt 2 || -z "$1" || ( $# == 2 && "$2" != --webrtc && "$2" != --pair && "$2" != --races ) ]]; then
    echo 'Usage: run_android_device_smoke.sh DEVICE_SERIAL [--webrtc|--pair|--races]' >&2
    exit 2
fi
serial=$1
adb_command=${P2PDC_ADB:-adb}
command -v "$adb_command" >/dev/null
for file in external_consumer p2pdc_echo native_lifecycle libc++_shared.so; do
    if [[ ! -f "$root/.deps/android-device-test/$file" ]]; then
        echo "Missing Android test binary: $file" >&2
        exit 1
    fi
done
if [[ ${2:-} == --webrtc && ! -f "$root/.deps/android-device-test/native_smoke" ]]; then
    echo 'Missing Android WebRTC smoke binary.' >&2
    exit 1
fi
if [[ ( ${2:-} == --pair || ${2:-} == --races ) && ! -f "$root/.deps/android-device-test/native_pair" ]]; then
    echo 'Missing Android public node pair binary.' >&2
    exit 1
fi
if [[ ! -x "$root/build-native/p2pdc_echo" ]]; then
    echo 'Build the native p2pdc_echo first.' >&2
    exit 1
fi
abi=$("$adb_command" -s "$serial" shell getprop ro.product.cpu.abilist | tr -d '\r')
sdk=$("$adb_command" -s "$serial" shell getprop ro.build.version.sdk | tr -d '\r')
if [[ ",$abi," != *,arm64-v8a,* || ! "$sdk" =~ ^[0-9]+$ ]] || (( sdk < 24 )); then
    echo 'The test requires an arm64 Android device with API level 24 or newer.' >&2
    exit 1
fi
remote=$("$adb_command" -s "$serial" shell 'mktemp -d /data/local/tmp/p2pdc-test.XXXXXX' | tr -d '\r')
if [[ ! "$remote" =~ ^/data/local/tmp/p2pdc-test\.[a-zA-Z0-9]+$ ]]; then
    echo 'Device did not create a safe temporary test directory.' >&2
    exit 1
fi
cleanup() {
    status=$?
    trap - EXIT
    if ! timeout 10 "$adb_command" -s "$serial" shell "rm -rf '$remote'"; then
        echo "Could not remove device test directory: $remote" >&2
        status=1
    fi
    exit "$status"
}
trap cleanup EXIT
for file in external_consumer p2pdc_echo native_lifecycle libc++_shared.so; do
    "$adb_command" -s "$serial" push "$root/.deps/android-device-test/$file" "$remote/" >/dev/null
done
if [[ ${2:-} == --webrtc ]]; then
    "$adb_command" -s "$serial" push "$root/.deps/android-device-test/native_smoke" "$remote/" >/dev/null
    "$adb_command" -s "$serial" shell "chmod 700 '$remote/native_smoke'"
fi
if [[ ${2:-} == --pair || ${2:-} == --races ]]; then
    "$adb_command" -s "$serial" push "$root/.deps/android-device-test/native_pair" "$remote/" >/dev/null
    "$adb_command" -s "$serial" shell "chmod 700 '$remote/native_pair'"
fi
"$adb_command" -s "$serial" shell "chmod 700 '$remote/external_consumer' '$remote/p2pdc_echo' '$remote/native_lifecycle'"
result=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' '$remote/external_consumer'" | tr -d '\r')
if [[ "$result" != 'external consumer passed' ]]; then
    echo "Unexpected Android consumer result: $result" >&2
    exit 1
fi
result=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' timeout 20 '$remote/native_lifecycle'" | tr -d '\r')
if [[ "$result" != 'native node lifecycle passed' ]]; then
    echo "Unexpected Android lifecycle result: $result" >&2
    exit 1
fi
seed=$(printf '01%.0s' {1..32})
expected=$(P2PDC_ECHO_SEED="$seed" "$root/build-native/p2pdc_echo" --public-key)
actual=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' P2PDC_ECHO_SEED='$seed' '$remote/p2pdc_echo' --public-key" | tr -d '\r')
if [[ "$actual" != "$expected" ]]; then
    echo 'Android and Linux signing public keys differ.' >&2
    exit 1
fi
zero_seed=$(printf '00%.0s' {1..32})
if invalid=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' P2PDC_ECHO_SEED='$zero_seed' '$remote/p2pdc_echo' --public-key" 2>&1); then
    echo 'Android accepted an invalid signing seed.' >&2
    exit 1
else
    status=$?
    if [[ "$status" != 2 || "$invalid" != *'must be nonzero'* ]]; then
        echo "Unexpected invalid-seed failure: $invalid" >&2
        exit 1
    fi
fi
if [[ ${2:-} == --webrtc ]]; then
    if ! result=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' timeout 35 '$remote/native_smoke' direct" | tr -d '\r'); then
        echo "Android WebRTC smoke exited unsuccessfully: $result" >&2
        exit 1
    fi
    if [[ "$result" != *'binary_echo=1'* || "$result" != *'route=Direct'* ]]; then
        echo "Android WebRTC smoke failed: $result" >&2
        exit 1
    fi
    echo 'Android local WebRTC echo passed'
fi
if [[ ${2:-} == --pair ]]; then
    if ! result=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' timeout 40 '$remote/native_pair'" | tr -d '\r'); then
        echo "Android public node pair exited unsuccessfully: $result" >&2
        exit 1
    fi
    if [[ "$result" != 'public node pair passed' ]]; then
        echo "Unexpected Android public node pair result: $result" >&2
        exit 1
    fi
    echo 'Android public node pair passed'
fi
if [[ ${2:-} == --races ]]; then
    if ! result=$("$adb_command" -s "$serial" shell "LD_LIBRARY_PATH='$remote' timeout 180 '$remote/native_pair' --races" | tr -d '\r'); then
        echo "Android public node races exited unsuccessfully: $result" >&2
        exit 1
    fi
    if [[ "$result" != 'public node pair passed' ]]; then
        echo "Unexpected Android public node races result: $result" >&2
        exit 1
    fi
    echo 'Android public node races passed'
fi
echo 'Android consumer and identity checks passed'
