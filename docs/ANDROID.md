# Android cross-build status

The portable core, discovery logic, and cryptographic components have been
cross-compiled and linked for Android arm64, API 24, using Android NDK
27.0.12077973 and an Android build of libsodium 1.0.22. A later partial build
included OpenDHT 4.4.1 and linked the same external C++ consumer used on native
and WebAssembly, including public `Node` identity creation. A subsequent full
arm64 build enabled both OpenDHT and the native WebRTC adapter and linked the
same C++ echo program used on Linux
and WebAssembly. The full-build consumer and a local WebRTC backend smoke test
have run on a device, as described below. Direct and relay-required connections
also passed between the phone and a Linux computer on different networks.

For Linux x86-64 hosts, the Android libsodium dependency can be built from the
verified 1.0.22 release. This was checked in a fresh temporary directory with
NDK 27.0.12077973 and an arm64 link probe. Set `P2PDC_ANDROID_NDK` and an
absolute `P2PDC_ANDROID_SODIUM` installation prefix before running:

```sh
mkdir -p .deps
curl -fL -o .deps/libsodium-1.0.22.tar.gz \
  https://download.libsodium.org/libsodium/releases/libsodium-1.0.22.tar.gz && \
  echo 'adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349  .deps/libsodium-1.0.22.tar.gz' | sha256sum -c - && \
  tar -xzf .deps/libsodium-1.0.22.tar.gz -C .deps
mkdir -p .deps/sodium-android-build
export P2PDC_ANDROID_TOOLS="$P2PDC_ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
cd .deps/sodium-android-build
PATH="$P2PDC_ANDROID_TOOLS:$PATH" CC=aarch64-linux-android24-clang \
  AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip CFLAGS='-Os -fPIC' \
  nice -n 15 ionice -c 3 ../libsodium-1.0.22/configure \
  --host=aarch64-linux-android --prefix="$P2PDC_ANDROID_SODIUM" \
  --enable-static --disable-shared --disable-pie
PATH="$P2PDC_ANDROID_TOOLS:$PATH" nice -n 15 ionice -c 3 make -j1
PATH="$P2PDC_ANDROID_TOOLS:$PATH" nice -n 15 ionice -c 3 make -j1 install
cd ../..
```

Given an Android-targeted libsodium installation, the partial check can be
repeated as follows. Set `P2PDC_ANDROID_NDK` to the NDK directory and
`P2PDC_ANDROID_SODIUM` to the libsodium installation prefix first:

```sh
cmake -S . -B build-android-core -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$P2PDC_ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DBUILD_TESTING=OFF -DP2PDC_ENABLE_OPENDHT=OFF \
  -DP2PDC_ENABLE_NATIVE_TRANSPORT=OFF -DP2PDC_ENABLE_SODIUM=ON \
  -DSODIUM_INCLUDE_DIR="$P2PDC_ANDROID_SODIUM/include" \
  -DSODIUM_LIBRARY="$P2PDC_ANDROID_SODIUM/lib/libsodium.a"
cmake --build build-android-core --parallel 1
```

A complete build needs patched Android libdatachannel and a compatible ICE
backend as described below. Supply an existing OpenDHT
CMake target through `P2PDC_OPENDHT_TARGET`, or configure `pkg-config` to use
Android-targeted packages. Do not link desktop libraries into an Android build.
Additional network conditions and suspension behavior still need validation.

## OpenDHT dependency preparation

[The isolated dependency-build recipe](ANDROID_OPENDHT_DEPS.md) downloads,
verifies, and builds the support libraries. OpenDHT and the discovery-only
consumer were built against that fresh prefix; the consumer ran on a phone. A
WebRTC echo also passed against Firefox using freshly rebuilt dependencies. See
[fresh WebRTC dependency notes](ANDROID_WEBRTC_DEPS.md) for the validation
scope; the end-to-end fresh-checkout procedure remains unverified.

As an initial cross-build check, Jami's upstream contrib recipes at commit
`66a64ee9a34c41d3aab1aa79af3d5568ece50326` built MessagePack C++ 7.0.0,
fmt 12.1.0, GMP 6.3.0, Nettle 4.0, and GnuTLS 3.8.13 for Android arm64,
API 24. Small programs using each dependency linked as AArch64 Android
executables; they were not run on a device. GnuTLS also used Android builds of
zlib and iconv. The sources and outputs are local under `.deps` and are not
library dependencies on Jami itself.

The upstream OpenDHT recipe at that commit selects OpenDHT 4.4.99 and includes
proxy-related components. Its complete build and compatibility with this
library have not been verified. The dependency builds used the Android NDK
27 toolchain, checksum-verified upstream archives, `make -j1`, and
`CMAKE_BUILD_PARALLEL_LEVEL=1`; the NDK's `llvm-ar`, `llvm-ranlib`,
`llvm-strip`, and `ld.lld` were selected explicitly. MessagePack's CMake
target supplies `MSGPACK_NO_BOOST`, which is also needed for a direct compiler
invocation when Boost is absent.

An initial GnuTLS attempt found a desktop TPM2 package through the host
`pkg-config` search path and failed to compile for Android. Cross-builds must
restrict both `PKG_CONFIG_LIBDIR` and `PKG_CONFIG_PATH` to the Android prefix;
never treat discovery of a host package as evidence of an Android dependency.
After cleaning and rebuilding with those restrictions, GnuTLS no longer enabled
TPM2; its small linked test required only Android's libc, libm, and libdl as
shared libraries. Upstream compilation emitted warnings.

A separate minimal OpenDHT 4.4.1 build at commit
`621b44c8e807e9d3b98794f6b5f8101ad216fd8f` disabled HTTP proxy and multicast peer
discovery and used target-prefixed packages. It linked an Android arm64
`DhtRunner` executable and the library's external consumer. These executables
depend on Android's `libc++_shared.so` as well as system libraries. The
consumer constructs a node and starts and stops its DHT on an Android device.
The full build described below has also passed Linux-to-Android internet tests.

Given the Android dependency prefix above, this pinned OpenDHT build and install
also succeeded in a fresh temporary directory. Set `P2PDC_ANDROID_DHT_INSTALL`
to an absolute installation prefix; this check did not rebuild the dependencies.

```sh
git clone https://github.com/savoirfairelinux/opendht.git .deps/opendht-android-src
git -C .deps/opendht-android-src checkout 621b44c8e807e9d3b98794f6b5f8101ad216fd8f
export PKG_CONFIG_LIBDIR="$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
nice -n 15 ionice -c 3 cmake -S .deps/opendht-android-src -B .deps/opendht-android-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$P2PDC_ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared \
  -DCMAKE_PREFIX_PATH="$P2PDC_ANDROID_PREFIX" -DCMAKE_FIND_ROOT_PATH="$P2PDC_ANDROID_PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$P2PDC_ANDROID_DHT_INSTALL" -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG' '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DOPENDHT_USE_PKGCONFIG=ON \
  -DOPENDHT_HTTP=OFF -DOPENDHT_PEER_DISCOVERY=OFF -DOPENDHT_PROXY_CLIENT=OFF \
  -DOPENDHT_PROXY_SERVER=OFF -DOPENDHT_TOOLS=OFF -DOPENDHT_EXAMPLES=OFF \
  -DOPENDHT_PYTHON=OFF -DOPENDHT_C=OFF -DOPENDHT_DOWNLOAD_DEPS=OFF
nice -n 15 ionice -c 3 cmake --build .deps/opendht-android-build --parallel 1
nice -n 15 ionice -c 3 cmake --install .deps/opendht-android-build
```

For the partial build below, install OpenDHT into the dependency prefix or
include both prefixes in CMake and target `pkg-config` search paths. The
original full-build check used one shared prefix.

With an Android-targeted OpenDHT installation and its static dependencies in
`P2PDC_ANDROID_PREFIX`, and the NDK and libsodium variables set as above, the
partial discovery build can be configured with:

```sh
export PKG_CONFIG_LIBDIR="$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
cmake -S . -B build-android-dht -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$P2PDC_ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared \
  -DCMAKE_FIND_ROOT_PATH="$P2PDC_ANDROID_PREFIX" \
  -DCMAKE_PREFIX_PATH="$P2PDC_ANDROID_PREFIX" -DPKG_CONFIG_ARGN=--static \
  -DBUILD_TESTING=OFF -DP2PDC_ENABLE_OPENDHT=ON \
  -DP2PDC_ENABLE_NATIVE_TRANSPORT=OFF -DP2PDC_ENABLE_SODIUM=ON \
  -DSODIUM_INCLUDE_DIR="$P2PDC_ANDROID_SODIUM/include" \
  -DSODIUM_LIBRARY="$P2PDC_ANDROID_SODIUM/lib/libsodium.a"
cmake --build build-android-dht --parallel 1
```

## WebRTC cross-build

The current workspace has experimental Android arm64 static builds of GLib
2.88.3, libnice 0.1.24, and the project's patched libdatachannel 0.23.2. GLib
was built without tests, introspection, translations, libmount, or SELinux;
libnice used GnuTLS and disabled GStreamer and UPnP. Their support archives
include target-built PCRE2, libffi, and a small libintl compatibility library.
The dependency builds used Meson and the Android NDK, with one Ninja job.
Libnice emitted upstream alignment warnings during compilation.

The Meson cross file used for GLib and libnice can be generated on Linux x86-64
with the NDK and target OpenDHT dependency prefixes described above. Set
`P2PDC_ANDROID_GLIB` and `P2PDC_ANDROID_NICE` to absolute installation prefixes;
the GLib prefix need not exist when generating the file:

```sh
python3 tools/android_meson_cross.py --ndk "$P2PDC_ANDROID_NDK" \
  --android-prefix "$P2PDC_ANDROID_PREFIX" --glib-prefix "$P2PDC_ANDROID_GLIB" \
  > .deps/android-arm64-meson.ini
```

The generator checks the compiler and target dependency paths and selects only
the Android and GLib package directories. Clear `PKG_CONFIG_PATH` when running
Meson unless it contains only target packages; it can otherwise override the
cross file. A generated file was used to compile a small arm64 program with
NDK 27; the full dependency bootstrap from a fresh checkout remains unverified.

The GLib 2.88.3 source at commit `43bc79ea8803e33c5eb368085e2d5906f9f98079`
was configured for Android with the generated cross file, a fresh installation
prefix, static libraries, release mode at optimization level 1, tests disabled,
and introspection, documentation, man pages, SELinux, libmount, xattr, sysprof,
DTrace, SystemTap, NLS, and libelf disabled. A fresh build and development-file
install succeeded using locally cached GLib subprojects and an existing
Android OpenDHT dependency prefix. A later build used freshly downloaded GLib
subprojects and a fresh OpenDHT prefix, as described in the linked WebRTC notes.

With that prefix available and the GLib source and its cached subprojects
already present, the tested targeted build is:

```sh
PKG_CONFIG_PATH= nice -n 15 ionice -c 3 meson setup .deps/glib-android-build \
  .deps/glib-android-src --cross-file "$PWD/.deps/android-arm64-meson.ini" \
  --prefix "$P2PDC_ANDROID_GLIB" --libdir lib --default-library static \
  --buildtype release -Doptimization=1 -Dtests=false -Dintrospection=disabled \
  -Ddocumentation=false -Dman-pages=disabled -Dselinux=disabled -Dlibmount=disabled \
  -Dxattr=false -Dsysprof=disabled -Ddtrace=disabled -Dsystemtap=disabled \
  -Dnls=disabled -Dlibelf=disabled --wrap-mode=nodownload
nice -n 15 ionice -c 3 ninja -C .deps/glib-android-build -j 1 \
  glib/libglib-2.0.a gobject/libgobject-2.0.a gthread/libgthread-2.0.a \
  gmodule/libgmodule-2.0.a gio/libgio-2.0.a girepository/libgirepository-2.0.a \
  subprojects/pcre2-10.46/libpcre2-8.a subprojects/pcre2-10.46/libpcre2-posix.a \
  subprojects/pcre2-10.46/libpcre2-16.a subprojects/pcre2-10.46/libpcre2-32.a \
  subprojects/libffi-3.5.2/src/libffi.a subprojects/proxy-libintl-0.5/libintl.a
nice -n 15 ionice -c 3 meson install -C .deps/glib-android-build --tags devel --no-rebuild
```

Given an already built Android GLib and the Android OpenDHT dependency prefix,
libnice 0.1.24 can be rebuilt using that file. The following targeted build and
development-file install were checked in a fresh temporary build directory; the
upstream default build also tries to link optional utilities and did not succeed
with these static dependency flags.

```sh
git clone --depth 1 --branch 0.1.24 https://github.com/libnice/libnice.git .deps/libnice-android-src
test "$(git -C .deps/libnice-android-src rev-parse HEAD)" = \
  af62014d520ac09c8fe1974bda1b253114c68843
PKG_CONFIG_PATH= nice -n 15 ionice -c 3 meson setup .deps/libnice-android-build \
  .deps/libnice-android-src --cross-file "$PWD/.deps/android-arm64-meson.ini" \
  --prefix "$P2PDC_ANDROID_NICE" --libdir lib --default-library static \
  --buildtype release -Doptimization=1 -Dgupnp=disabled -Dgstreamer=disabled \
  -Dcrypto-library=gnutls -Dexamples=disabled -Dtests=disabled \
  -Dgtk_doc=disabled -Dintrospection=disabled --wrap-mode=nodownload
nice -n 15 ionice -c 3 ninja -C .deps/libnice-android-build -j 1 nice/libnice.a
nice -n 15 ionice -c 3 meson install -C .deps/libnice-android-build --tags devel --no-rebuild
```

With the patched libdatachannel checkout from the main README, and the Android
OpenDHT, GLib, and libnice prefixes prepared, its static libraries can be built
as follows. A fresh temporary build and install using these options succeeded
with NDK 27. Set `P2PDC_ANDROID_DATACHANNEL` to its installation prefix and use
absolute paths for all prefixes and the NDK.

```sh
export PKG_CONFIG_LIBDIR="$P2PDC_ANDROID_NICE/lib/pkgconfig:$P2PDC_ANDROID_GLIB/lib/pkgconfig:$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
P2PDC_ANDROID_PREFIXES="$P2PDC_ANDROID_NICE;$P2PDC_ANDROID_GLIB;$P2PDC_ANDROID_PREFIX"
cmake -S .deps/libdatachannel -B .deps/libdatachannel-android-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$P2PDC_ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared \
  -DCMAKE_FIND_ROOT_PATH="$P2PDC_ANDROID_PREFIXES" -DCMAKE_PREFIX_PATH="$P2PDC_ANDROID_PREFIXES" \
  -DPKG_CONFIG_ARGN=--static -DCMAKE_INSTALL_PREFIX="$P2PDC_ANDROID_DATACHANNEL" \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG' \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' -DBUILD_SHARED_LIBS=OFF \
  -DUSE_NICE=ON -DUSE_GNUTLS=ON -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON
nice -n 15 ionice -c 3 cmake --build .deps/libdatachannel-android-build --parallel 1
nice -n 15 ionice -c 3 cmake --install .deps/libdatachannel-android-build
```

A checked-in CMake helper, `cmake/AndroidStaticDependencies.cmake`, supplies
libdatachannel, SCTP, libnice, and their target-only static dependencies to the
external consumer build. With OpenDHT, libsodium, and both native
backends enabled, `build-android-full/external_consumer` and
`build-android-full/p2pdc/p2pdc_echo` linked successfully for arm64. The latter
contains the patched libdatachannel feature symbol. A single-threaded linker
with a 3 GiB virtual-memory limit was needed for the final link; a 1.5 GiB
limit was insufficient. No system libraries were installed or replaced.

On a Pixel 6a, the foreground device smoke test passed the public consumer,
identity checks, local DHT startup and shutdown without bootstrap peers, and a
direct in-process libdatachannel binary echo using GLib and libnice. Two public
nodes on the same phone also discovered one another through a loopback DHT,
connected, exchanged authenticated binary messages twice, and shut down. The
test uses the same C++ consumer as the Linux and browser tests. A separate
Linux-to-Pixel tests on different networks used Jami's public OpenDHT bootstrap
and Google's public STUN service; the standalone C++ echo peers exchanged
authenticated data in both connection roles and reported a direct route. With
Jami's public TURN server, both relay-required roles also exchanged data across
the internet and reported local and remote relayed candidates. Mixed-policy
tests passed with either Android or Linux requiring its own relay; the automatic
peer used a direct local candidate and observed the remote relay. With direct ICE
checks blocked on an isolated Linux network, Android automatically selected its
own relay and exchanged data in both connection roles. A no-TURN control reached
connection setup but timed out. A complete dependency bootstrap and app
suspension checks remain needed. These runs do not establish reliable operation
across all internet networks.

Separate internet tests also exchanged direct and relay-required echoes between
the phone and a browser using Jami's HTTPS DHT proxy; see
[browser testing](BROWSER_TESTING.md) for the tested combinations and commands.

Given those target-built dependency prefixes, the external consumer and echo
program can be rebuilt as follows. Set `P2PDC_ANDROID_NICE`,
`P2PDC_ANDROID_GLIB`, and `P2PDC_ANDROID_DATACHANNEL` to the libnice, GLib,
and patched libdatachannel prefixes; set the OpenDHT, libsodium, and NDK
variables described above as well.

```sh
export PKG_CONFIG_LIBDIR="$P2PDC_ANDROID_NICE/lib/pkgconfig:$P2PDC_ANDROID_GLIB/lib/pkgconfig:$P2PDC_ANDROID_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
cmake -S tests/external_consumer -B build-android-full -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$P2PDC_ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="$PWD/cmake/AndroidStaticDependencies.cmake" \
  -DP2PDC_ANDROID_DATACHANNEL_PREFIX="$P2PDC_ANDROID_DATACHANNEL" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared \
  -DCMAKE_FIND_ROOT_PATH="$P2PDC_ANDROID_NICE;$P2PDC_ANDROID_GLIB;$P2PDC_ANDROID_PREFIX" \
  -DCMAKE_PREFIX_PATH="$P2PDC_ANDROID_NICE;$P2PDC_ANDROID_GLIB;$P2PDC_ANDROID_PREFIX" \
  -DCMAKE_EXE_LINKER_FLAGS='-Wl,--threads=1' -DPKG_CONFIG_ARGN=--static \
  -DP2PDC_SOURCE_DIR="$PWD" -DBUILD_TESTING=OFF -DP2PDC_ANDROID_BACKEND_SMOKE=ON \
  -DP2PDC_NATIVE_LIFECYCLE_SMOKE=ON -DP2PDC_NATIVE_PAIR_SMOKE=ON \
  -DSODIUM_INCLUDE_DIR="$P2PDC_ANDROID_SODIUM/include" \
  -DSODIUM_LIBRARY="$P2PDC_ANDROID_SODIUM/lib/libsodium.a"
nice -n 15 ionice -c 3 cmake --build build-android-full \
  --target external_consumer p2pdc_echo p2pdc_native_lifecycle p2pdc_native_pair \
  p2pdc_android_backend_smoke --parallel 1
```

The helper requires target-only `PKG_CONFIG_LIBDIR` and `PKG_CONFIG_PATH`,
but cannot itself prove that every supplied archive has the correct ABI.
`bash tests/android_static_dependencies_test.sh` checks its rejection paths.

Prepare the device payload from the built executables and the matching NDK
runtime (the host toolchain path below is for Linux x86-64):

```sh
P2PDC_ANDROID_TOOLS="$P2PDC_ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
mkdir -p .deps/android-device-test
"$P2PDC_ANDROID_TOOLS/bin/llvm-strip" -o .deps/android-device-test/external_consumer build-android-full/external_consumer
"$P2PDC_ANDROID_TOOLS/bin/llvm-strip" -o .deps/android-device-test/p2pdc_echo build-android-full/p2pdc/p2pdc_echo
"$P2PDC_ANDROID_TOOLS/bin/llvm-strip" -o .deps/android-device-test/native_lifecycle build-android-full/p2pdc_native_lifecycle
"$P2PDC_ANDROID_TOOLS/bin/llvm-strip" -o .deps/android-device-test/native_pair build-android-full/p2pdc_native_pair
"$P2PDC_ANDROID_TOOLS/bin/llvm-strip" -o .deps/android-device-test/native_smoke build-android-full/p2pdc_android_backend_smoke
cp "$P2PDC_ANDROID_TOOLS/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" .deps/android-device-test/
```

Build the Linux `p2pdc_echo` as described in the main build instructions.
Connect an authorized Android arm64 device with API level 24 or newer and run:

```sh
adb devices
bash tests/run_android_device_smoke.sh DEVICE_SERIAL
bash tests/run_android_device_smoke.sh DEVICE_SERIAL --webrtc
bash tests/run_android_device_smoke.sh DEVICE_SERIAL --pair
bash tests/run_android_device_smoke.sh DEVICE_SERIAL --races
```

The script creates a unique directory under `/data/local/tmp`, runs the public
consumer, starts and stops a local DHT without bootstrap peers, compares a
disposable signing key with Linux, checks invalid-seed rejection, and removes
the directory. It reports cleanup failure. The optional
`--webrtc` mode additionally runs the native backend's in-process data-channel
echo. `--pair` runs the public C++ two-node connection test with local DHT
bootstrap. `--races` runs simultaneous dialing, cancellation, rejection, cancellation
after acceptance, and concurrent requests on a persistent listener between two
peers on the phone. These modes require a usable
phone network interface and do not exercise a connection to another device.
The harness itself can be checked without a device using
`bash tests/android_device_smoke_test.sh`.

On 2026-11-16, a freshly rebuilt arm64 test payload passed the consumer,
identity, local WebRTC echo, public node pair, and all five race scenarios on
a Pixel 6a running API level 37. The persistent-listener case checks two
concurrent requests; it is not a multi-device or application-lifecycle test.
The rebuilt phone and Linux echo peers also exchanged data in both connection
roles using the public bootstrap and STUN services; both endpoints reported a
direct host-candidate route. The network topology was not established by this
run, and TURN was not tested in it.
Chromium and the phone also exchanged data in both connection roles through
the public browser discovery proxy. Chromium reported a direct route; the
phone reported an unknown route for its host/peer-reflexive candidate pair.

The internet echo test is opt-in: it contacts the public Jami OpenDHT network
and Google STUN, exposing the peers' network addresses to those services. It
generates fresh disposable identities and capability values each time, and
removes its temporary phone files. Connect the devices through different
networks and run:

```sh
bash tests/run_android_internet_echo.sh DEVICE_SERIAL phone-responder
bash tests/run_android_internet_echo.sh DEVICE_SERIAL phone-initiator
```

The harness's input and cleanup failure paths can be checked offline with
`bash tests/android_internet_harness_test.sh`. A direct connection can fail
under NAT or firewall conditions without a configured TURN relay.

To test relay-required or mixed policies, provide an available TURN service in
the environment. The `relay` mode requires both peers to use a local relay;
`phone-auto` and `linux-auto` put the named peer in automatic mode and the other
in relay-required mode. The harness checks that each relay-required peer reports
its local relay, and prints both route observations. Credentials in this test
must use simple ASCII letters, digits, and `._:+/=-` characters.

```sh
export P2PDC_TEST_TURN_HOST='<TURN host>' P2PDC_TEST_TURN_PORT=3478
export P2PDC_TEST_TURN_USER='<temporary username>' P2PDC_TEST_TURN_PASSWORD='<temporary password>'
export P2PDC_TEST_TURN_TRANSPORT=udp
bash tests/run_android_internet_echo.sh DEVICE_SERIAL phone-responder relay
bash tests/run_android_internet_echo.sh DEVICE_SERIAL phone-initiator phone-auto
bash tests/run_android_internet_echo.sh DEVICE_SERIAL phone-responder linux-auto
```

The successful tests used the [Jami TURN server](https://docs.jami.net/en_US/developer/going-further/setting-up-your-own-turn-server.html).
The test service observes network addresses and relays encrypted traffic; no
default TURN service or credentials are built into the library.

For the automatic Android local-fallback test, Linux needs `unshare`, `nsenter`,
`slirp4netns`, `nft`, and access to `/dev/net/tun`. The test creates a temporary
user and network namespace, blocks direct STUN-format ICE checks there, verifies
the phone's selected local relay, then runs a no-TURN control. Its rules do not
modify the host firewall. With the same TURN environment variables, run:

```sh
bash tests/run_android_forced_fallback.sh DEVICE_SERIAL phone-initiator
bash tests/run_android_forced_fallback.sh DEVICE_SERIAL phone-responder
```

The filter permits UDP to the resolved TURN and Google STUN servers; it is a
test of these specific network conditions, not proof of firewall enforcement or
relay-only privacy under all conditions.
