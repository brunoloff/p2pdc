#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-"$root/build-native-tls/p2pdc_native_transport_tests"}
system_nice=${P2PDC_TEST_UNPATCHED_NICE_DIR:-$(pkg-config --variable=libdir nice)}
if [[ $# -gt 1 || ! -x "$binary" || ! -f "$system_nice/libnice.so.10" ]]; then
    echo 'Usage: run_native_tls_feature_check.sh [NATIVE_TEST_BINARY]' >&2
    exit 2
fi
if nm -D "$system_nice/libnice.so.10" | grep -F 'nice_p2pdc_tls_supported' >/dev/null; then
    echo 'P2PDC_TEST_UNPATCHED_NICE_DIR must identify an unpatched libnice.' >&2
    exit 1
fi
test_dir=$(mktemp -d /tmp/p2pdc-tls-feature.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
printf '%s\n' 'unsigned int nice_p2pdc_tls_supported(void) { return 1; }' >"$test_dir/fake.c"
${CC:-cc} -shared -fPIC "$test_dir/fake.c" -o "$test_dir/fake.so"
export LD_PRELOAD="$test_dir/fake.so${LD_PRELOAD:+:$LD_PRELOAD}"
filter='NativeIceConfig.UsesTheLoadedBackendTlsCapability'
P2PDC_TEST_EXPECT_TLS=yes "$binary" --gtest_filter="$filter" --gtest_fail_if_no_test_selected
P2PDC_TEST_EXPECT_TLS=no LD_LIBRARY_PATH="$system_nice${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$binary" --gtest_filter="$filter" --gtest_fail_if_no_test_selected
echo 'TLS capability follows the active libnice, not an unrelated marker'
