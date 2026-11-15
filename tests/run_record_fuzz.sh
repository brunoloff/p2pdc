#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d /tmp/p2pdc-record-fuzz.XXXXXX)
cleanup() {
    exit_code=$?
    if (( exit_code == 0 )); then rm -rf -- "$test_dir"; else echo "Fuzz artifacts retained in $test_dir" >&2; fi
}
trap cleanup EXIT
mkdir "$test_dir/corpus"
python3 - "$root" "$test_dir/corpus" <<'PY'
from pathlib import Path
import sys
from nacl.public import PrivateKey, SealedBox

root, corpus = map(Path, sys.argv[1:])
def vector(name):
    return bytes.fromhex(''.join(line.strip()[1:-1] for line in (root / 'tests' / name).read_text().splitlines()))
(corpus / 'advertisement').write_bytes(b'\0' + vector('advertisement_vector.inc'))
plaintext = vector('signaling_vector.inc')
recipient = PrivateKey(bytes([17]) + bytes(31))
(corpus / 'signaling').write_bytes(b'\1PDSG\1' + SealedBox(recipient.public_key).encrypt(plaintext))
(corpus / 'candidate').write_bytes(b'\2PDIC\1\0\x09\0\x01candidate0')
(corpus / 'handshake').write_bytes(b'\3' + vector('handshake_vector.inc'))
(corpus / 'truncated').write_bytes(b'\0P')
PY
nice -n 15 ionice -c 3 "${P2PDC_FUZZ_CXX:-clang++}" -std=c++20 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined -I "$root/include" \
    "$root/tests/fuzz_records.cpp" "$root/src/core/memory_budget.cpp" \
    "$root/src/security/security.cpp" "$root/src/security/recipient_crypto.cpp" \
    "$root/src/security/signaling.cpp" "$root/src/security/signaling_candidate.cpp" \
    "$root/src/security/handshake.cpp" "$root/src/discovery/advertisement.cpp" \
    -lsodium -o "$test_dir/fuzz_records"
nice -n 15 ionice -c 3 "$test_dir/fuzz_records" -runs=10000 -max_len=4096 -rss_limit_mb=512 \
    -artifact_prefix="$test_dir/" "$test_dir/corpus"
