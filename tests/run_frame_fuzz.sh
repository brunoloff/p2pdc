#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-frame-fuzz.XXXXXX)
cleanup() {
    exit_code=$?
    if (( exit_code == 0 )); then rm -rf -- "$test_dir"; else echo "Fuzz artifacts retained in $test_dir" >&2; fi
}
trap cleanup EXIT
mkdir "$test_dir/corpus"
python3 - "$test_dir/corpus" <<'PY'
import pathlib
import struct
import sys

directory = pathlib.Path(sys.argv[1])
limits = struct.pack('>II', 1024, 1024)
(directory / 'valid-empty').write_bytes(limits + b'PD\x01' + struct.pack('>QII', 1, 0, 0))
(directory / 'valid-data').write_bytes(limits + b'PD\x01' + struct.pack('>QII', 2, 3, 0) + b'\x00\x80\xff')
(directory / 'truncated').write_bytes(limits + b'PD')
(directory / 'invalid-bounds').write_bytes(limits + b'PD\x01' + struct.pack('>QII', 3, 1, 2) + b'x')
PY
nice -n 15 ionice -c 3 clang++ -std=c++20 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -I "$root/include" \
    "$root/tests/fuzz_frame.cpp" "$root/src/protocol/frame.cpp" -o "$test_dir/fuzz_frame"
nice -n 15 ionice -c 3 "$test_dir/fuzz_frame" -runs=10000 -max_len=4096 -rss_limit_mb=512 \
    -artifact_prefix="$test_dir/" "$test_dir/corpus"
