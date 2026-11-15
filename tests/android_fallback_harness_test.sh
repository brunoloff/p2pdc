#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-fallback-harness.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
for scenario in missing invalid-role; do
    args=()
    if [[ "$scenario" == invalid-role ]]; then args=(test-device invalid); fi
    if bash "$root/tests/run_android_forced_fallback.sh" "${args[@]}" >"$test_dir/log" 2>&1; then
        echo "Unexpected success for $scenario" >&2; exit 1
    else
        exit_code=$?
        if [[ "$exit_code" != 2 ]] || ! grep -q '^Usage:' "$test_dir/log"; then
            cat "$test_dir/log" >&2; exit 1
        fi
    fi
done
if P2PDC_TEST_BROWSER_RUNNER="$test_dir/missing" bash "$root/tests/run_android_forced_fallback.sh" test-device >"$test_dir/log" 2>&1; then
    echo 'Missing browser runner was accepted.' >&2; exit 1
fi
grep -q 'Browser test runner is not executable' "$test_dir/log"
echo 'Android fallback harness argument checks passed'
