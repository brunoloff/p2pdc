#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-android-cmake.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
module="$root/cmake/AndroidStaticDependencies.cmake"
for scenario in host missing-prefix missing-files no-static missing-pkg-path; do
    case "$scenario" in
        host) args=(-DCMAKE_SYSTEM_NAME=Linux); expected='requires an Android toolchain' ;;
        missing-prefix) args=(-DCMAKE_SYSTEM_NAME=Android); expected='Set P2PDC_ANDROID_DATACHANNEL_PREFIX' ;;
        missing-files)
            args=(-DCMAKE_SYSTEM_NAME=Android "-DP2PDC_ANDROID_DATACHANNEL_PREFIX=$test_dir")
            expected='Missing Android libdatachannel file' ;;
        no-static)
            mkdir -p "$test_dir/lib" "$test_dir/include/rtc"
            touch "$test_dir/lib/libdatachannel.a" "$test_dir/lib/libusrsctp.a" "$test_dir/include/rtc/global.hpp"
            args=(-DCMAKE_SYSTEM_NAME=Android "-DP2PDC_ANDROID_DATACHANNEL_PREFIX=$test_dir")
            expected='PKG_CONFIG_ARGN to include --static' ;;
        missing-pkg-path)
            args=(-DCMAKE_SYSTEM_NAME=Android "-DP2PDC_ANDROID_DATACHANNEL_PREFIX=$test_dir" -DPKG_CONFIG_ARGN=--static)
            expected='Set PKG_CONFIG_LIBDIR and PKG_CONFIG_PATH' ;;
    esac
    if env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_PATH cmake "${args[@]}" -P "$module" >"$test_dir/log" 2>&1; then
        echo "Unexpected configuration success: $scenario" >&2; exit 1
    fi
    if ! grep -q "$expected" "$test_dir/log"; then cat "$test_dir/log" >&2; exit 1; fi
done
if cmake -S "$root/tests/external_consumer" -B "$test_dir/host-build" \
    -DP2PDC_SOURCE_DIR="$root" -DP2PDC_ANDROID_BACKEND_SMOKE=ON >"$test_dir/log" 2>&1; then
    echo 'Unexpected Android smoke configuration success on host' >&2; exit 1
fi
if ! grep -q 'P2PDC_ANDROID_BACKEND_SMOKE requires an Android toolchain' "$test_dir/log"; then
    cat "$test_dir/log" >&2; exit 1
fi
if cmake -S "$root/tests/external_consumer" -B "$test_dir/browser-build" \
    -DP2PDC_SOURCE_DIR="$root" -DEMSCRIPTEN=ON -DP2PDC_NATIVE_LIFECYCLE_SMOKE=ON >"$test_dir/log" 2>&1; then
    echo 'Unexpected native lifecycle configuration success on browser' >&2; exit 1
fi
if ! grep -q 'P2PDC_NATIVE_LIFECYCLE_SMOKE requires a native target' "$test_dir/log"; then
    cat "$test_dir/log" >&2; exit 1
fi
if cmake -S "$root/tests/external_consumer" -B "$test_dir/browser-pair-build" \
    -DP2PDC_SOURCE_DIR="$root" -DEMSCRIPTEN=ON -DP2PDC_NATIVE_PAIR_SMOKE=ON >"$test_dir/log" 2>&1; then
    echo 'Unexpected native pair configuration success on browser' >&2; exit 1
fi
if ! grep -q 'P2PDC_NATIVE_PAIR_SMOKE requires a native target' "$test_dir/log"; then
    cat "$test_dir/log" >&2; exit 1
fi
echo 'Android dependency validation checks passed'
