# Android OpenDHT support dependencies

The OpenDHT Android build needs target builds of Argon2, fmt, GMP, GnuTLS,
Nettle, MessagePack C++, iconv, and zlib. The following Linux x86-64 procedure
was checked with NDK 27.0.12077973, API 24, and the pinned Jami contrib
recipes. It downloaded and checksum-verified all eight source archives, then
built them in a fresh isolated prefix with one job. OpenDHT built against that
prefix. This library's discovery-only consumer linked and ran on a phone. A
WebRTC-enabled echo also linked against the fresh prefix and exchanged data with
Firefox over the internet. A later check rebuilt its Android WebRTC dependencies
and libsodium from freshly fetched sources; see [the WebRTC notes](ANDROID_WEBRTC_DEPS.md).

Set `P2PDC_ANDROID_NDK` and an absolute, empty `P2PDC_ANDROID_PREFIX`. The
build also needs Git, CMake, Ninja, GNU
make, pkg-config, Autoconf, Automake, libtool, m4, patch, and standard build
utilities. Run from the library's root directory:

```sh
git clone https://github.com/savoirfairelinux/jami-daemon.git .deps/jami-daemon
git -C .deps/jami-daemon checkout 66a64ee9a34c41d3aab1aa79af3d5568ece50326
mkdir -p .deps/jami-daemon/contrib/tarballs .deps/jami-daemon/contrib/repro
mkdir -p "$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
(
  set -eu
  cd .deps/jami-daemon/contrib/repro
  ANDROID_NDK="$P2PDC_ANDROID_NDK" ANDROID_ABI=arm64-v8a ANDROID_API=24 \
    ../bootstrap --host=aarch64-linux-android --prefix="$P2PDC_ANDROID_PREFIX" \
      --disable-all --enable-argon2 --enable-fmt --enable-msgpack --enable-gnutls \
      --ignore-system-libs
  export PATH="$P2PDC_ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
  export PKG_CONFIG_LIBDIR="$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
  export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
  export CMAKE_BUILD_PARALLEL_LEVEL=1 MAKEFLAGS=-j1
  export CFLAGS=-O1 CXXFLAGS=-O1 LDFLAGS='-Wl,--threads=1'
  nice -n 15 ionice -c 3 make -j1 PKG_CONFIG=pkg-config fetch
  nice -n 15 ionice -c 3 make -j1 \
    CC=aarch64-linux-android24-clang CXX=aarch64-linux-android24-clang++ \
    LD=ld.lld AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip \
    'CCAS=aarch64-linux-android24-clang -c' PKG_CONFIG=pkg-config
)
```

The `CFLAGS` and `CXXFLAGS` values are exported, not passed as make command-line
assignments: the latter override flags needed by the Argon2 recipe. The NDK
compiler's `-c` flag is needed for GMP's assembler configuration. Keep both
pkg-config paths target-only; a host package can silently contaminate a cross
build. Continue with the pinned OpenDHT build in [Android testing](ANDROID.md).
