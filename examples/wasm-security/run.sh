#!/usr/bin/env bash
set -euo pipefail

# Build only in a fresh temporary directory; preserve it for inspection.
emscripten_root=${P2PDC_EMSCRIPTEN_ROOT:-/usr/lib/emscripten}
export EM_CACHE=/tmp/p2pdc-emcache
for executable in git make node python3 mktemp sha256sum tee; do
    if ! command -v "$executable" >/dev/null; then
        echo "Missing required executable: $executable" >&2
        exit 2
    fi
done
for executable in emconfigure emmake em++; do
    if [[ ! -x "$emscripten_root/$executable" ]]; then
        echo "Missing Emscripten executable: $emscripten_root/$executable" >&2
        exit 2
    fi
done
emscripten_root=$(cd -- "$emscripten_root" && pwd -P)
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sources=(tests/security_test.cpp tests/recipient_crypto_test.cpp tests/advertisement_test.cpp
    tests/discovery_key_test.cpp tests/signaling_test.cpp src/security/security.cpp
    src/security/recipient_crypto.cpp src/security/signaling.cpp src/discovery/advertisement.cpp
    src/discovery/discovery_key.cpp)
for source in "${sources[@]}" include/p2pdc/security.h; do
    if [[ ! -r "$project_root/$source" ]]; then
        echo "Missing project source: $project_root/$source" >&2
        exit 2
    fi
done
build_root=$(mktemp -d /tmp/p2pdc-wasm-security.XXXXXX)
sodium_root="$build_root/libsodium"
sodium_prefix="$build_root/sodium-install"
gtest_root="$build_root/googletest"
printf 'Build directory (preserved): %s\n' "$build_root"
for source in "${sources[@]}"; do
    sha256sum "$project_root/$source"
done > "$build_root/source-sha256.txt"
sha256sum "$project_root"/include/p2pdc/*.h >> "$build_root/source-sha256.txt"
"$emscripten_root/em++" --version > "$build_root/tool-versions.txt"
node --version >> "$build_root/tool-versions.txt"

git clone --depth 1 --branch 1.0.22-RELEASE https://github.com/jedisct1/libsodium.git \
    "$sodium_root" > "$build_root/sodium-clone.log" 2>&1
if [[ $(git -C "$sodium_root" rev-parse HEAD) != 77e1ce5d6dee871c49ef211222ba18ef0c486bda ]]; then
    echo "Unexpected libsodium release commit; inspect $build_root/sodium-clone.log" >&2
    exit 2
fi
git clone --depth 1 --branch v1.17.0 https://github.com/google/googletest.git \
    "$gtest_root" > "$build_root/gtest-clone.log" 2>&1
if [[ $(git -C "$gtest_root" rev-parse HEAD) != 52eb8108c5bdec04579160ae17225d66034bd723 ]]; then
    echo "Unexpected GoogleTest release commit; inspect $build_root/gtest-clone.log" >&2
    exit 2
fi
(
    cd "$sodium_root"
    "$emscripten_root/emconfigure" ./configure --host=wasm32-unknown-emscripten \
        --prefix="$sodium_prefix" --disable-shared --enable-static --disable-asm \
        --disable-pie --without-pthreads CFLAGS=-O2 > "$build_root/sodium-configure.log" 2>&1
    "$emscripten_root/emmake" make -j 1 > "$build_root/sodium-build.log" 2>&1
    "$emscripten_root/emmake" make install > "$build_root/sodium-install.log" 2>&1
)
compile_flags=(-std=c++20 -O2 -fexceptions -DGTEST_HAS_PTHREAD=0)
include_flags=(-I"$gtest_root/googletest/include" -I"$sodium_prefix/include" -I"$project_root/include")
{
    "$emscripten_root/em++" "${compile_flags[@]}" "${include_flags[@]}" \
        -I"$gtest_root/googletest" -c "$gtest_root/googletest/src/gtest-all.cc" \
        -o "$build_root/gtest-all.o"
    "$emscripten_root/em++" "${compile_flags[@]}" "${include_flags[@]}" \
        -c "$gtest_root/googletest/src/gtest_main.cc" -o "$build_root/gtest-main.o"
    objects=()
    for source in "${sources[@]}"; do
        object="$build_root/${source//\//_}.o"
        "$emscripten_root/em++" "${compile_flags[@]}" "${include_flags[@]}" \
            -c "$project_root/$source" -o "$object"
        objects+=("$object")
    done
    "$emscripten_root/em++" -O2 -fexceptions -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 \
        "$build_root/gtest-all.o" "$build_root/gtest-main.o" \
        "${objects[@]}" \
        "$sodium_prefix/lib/libsodium.a" -o "$build_root/security-test.js"
} > "$build_root/test-build.log" 2>&1
sha256sum --check "$build_root/source-sha256.txt"
node "$build_root/security-test.js" --gtest_color=no | tee "$build_root/test-results.log"
printf 'Artifacts and logs preserved at %s\n' "$build_root"
