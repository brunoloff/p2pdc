#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-fuzz-harness.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
printf '%s\n' '#!/bin/sh' 'echo simulated-compiler-failure >&2' 'exit 1' > "$test_dir/clang++"
chmod +x "$test_dir/clang++"
if PATH="$test_dir:$PATH" bash "$root/tests/run_frame_fuzz.sh" >"$test_dir/log" 2>&1; then
    echo 'Compiler failure was ignored.' >&2; exit 1
fi
grep -q 'simulated-compiler-failure' "$test_dir/log"
artifact_dir=$(sed -n 's/^Fuzz artifacts retained in //p' "$test_dir/log")
if [[ ! "$artifact_dir" =~ ^/tmp/p2pdc-frame-fuzz\.[a-zA-Z0-9]+$ || ! -d "$artifact_dir" ]]; then
    echo 'Fuzz artifacts were not retained safely.' >&2; exit 1
fi
rm -rf -- "$artifact_dir"
echo 'Fuzz harness failure check passed'
