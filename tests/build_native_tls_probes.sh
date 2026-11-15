#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=${P2PDC_NICE_SOURCE:-"$root/.deps/libnice-tls-src"}
prefix=${P2PDC_NICE_PREFIX:-"$root/.deps/libnice-tls-install"}
if [[ $# != 0 || ! -f "$source_dir/socket/tls.c" || ! -f "$prefix/lib/libnice.so" ]]; then
    echo 'Build the patched libnice before running this script.' >&2
    exit 2
fi
mkdir -p "$root/.deps"
read -r -a tls_flags <<<"$(pkg-config --cflags --libs gio-2.0 openssl)"
nice -n 15 ionice -c 3 "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I "$source_dir" -I "$source_dir/agent" -I "$source_dir/socket" -I "$source_dir/stun" \
    -I "$source_dir/random" "$root/tests/native_tls_socket_unit.c" -o "$root/.deps/native_tls_socket_unit" \
    "${tls_flags[@]}"
read -r -a nice_flags <<<"$(pkg-config --cflags --libs gio-2.0)"
nice -n 15 ionice -c 3 "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -I "$prefix/include" "$root/tests/native_turn_tls_probe.cpp" -o "$root/.deps/native_turn_tls_probe" \
    -L "$prefix/lib" -Wl,-rpath,"$prefix/lib" -lnice "${nice_flags[@]}"
