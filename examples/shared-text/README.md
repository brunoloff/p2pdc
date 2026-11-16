# Shared Text example

A multi-person plain-text editor using the public p2pdc API, Qt Quick and the Yrs
C interface. Each side has its own Yrs document. Peers exchange Yrs updates
and merge concurrent edits; on reconnection each sends its current state to
catch up. The same C++ application and QML are used for each target. Yrs is a
dependency of this example, not of the p2pdc transport API.

Each participant gets a random cursor color and can change it using the
`#RRGGBB` field while editing. Cursor positions and colors are sent roughly
once a second as temporary application messages, not Yrs document updates.
They use the existing reliable channel, so updates can be delayed under
congestion; positions are approximate while documents are out of sync. Cursor
markers disappear after a peer disconnects or stops sending updates.

The example keeps its document in memory. It sends a full Yrs catch-up update
when a connection becomes ready, then sends incremental updates relative to
what it has queued on that connection. On backpressure it retains the old
baseline and retries; reconnecting starts a fresh catch-up. The negotiated
p2pdc message size limit bounds the practical catch-up size. If an update is too large,
the status reports that it could not be synchronized. NUL and invalid UTF-16
are rejected; the local text is limited to 65,536 UTF-16 code units. This is a
demonstration, not a durable or production document service.

## Build

The example is enabled by default (`P2PDC_BUILD_SHARED_TEXT=ON`) when p2pdc is
the top-level CMake project. It needs Qt 6
Core, Gui, Qml, Quick and Quick Controls 2, plus a target-compatible Yrs C
library and its matching `libyrs.h`. The Linux development checks used Qt
6.11.2 and Yrs 0.28.0. Build Yrs once with Cargo as in the
[Yrs reference instructions](../../integrations/yrs/README.md), then configure
p2pdc with its normal dependencies and the two Yrs paths:

```sh
cmake -S . -B build-native -G Ninja \
  -DLibDataChannel_DIR="$PWD/.deps/install/lib/cmake/LibDataChannel" \
  -DP2PDC_YRS_INCLUDE_DIR="$PWD/.deps/y-crdt/tests-ffi/include" \
  -DP2PDC_YRS_LIBRARY="$PWD/.deps/y-crdt-target/debug/libyrs.so"
nice -n 15 ionice -c 3 cmake --build build-native --parallel 1 --target p2pdc_shared_text
./bin/p2pdc_shared_text
```

The launcher prefers `build-native-tls` when available, otherwise it uses
`build-native`. The local paths are discovered automatically if they exist. Set
`-DP2PDC_BUILD_SHARED_TEXT=OFF` to build p2pdc without Qt or Yrs. When a
consumer embeds p2pdc with `add_subdirectory()` the example defaults to OFF;
the consumer can set the option before that call to build it.

For cross-compilation, use the target's Qt 6 kit and CMake toolchain, and pass
headers and a **target-architecture** Yrs library through the same two options.
Native host libraries cannot be linked into a WebAssembly or Android build.
The library itself currently validates Linux and browser, has partial Android
support, and does not yet have validated Windows, macOS or iOS backends.

## WebAssembly build

Use the same Qt version for the host and WebAssembly kits, with the Emscripten
version specified by that Qt kit. Qt 6.11.2 and Emscripten 4.0.7 were used for
the browser startup and editing smoke test. First install the Rust target with
`rustup target add wasm32-unknown-emscripten`. Activate Emscripten, then build
libsodium for that same SDK as described in the
[WebAssembly notes](../../docs/WEBASSEMBLY_INTERNALS.md). Use a separate output
directory if you already have a libsodium build for a different SDK.

Yrs 0.28.0's C binding needs the accompanying patch for this target: its undo
options exclude Emscripten even though the system clock is available there,
and its dynamic-library target is unnecessary for this static application.
Apply it to a fresh copy of the pinned Yrs source, not an existing native build:

```sh
git clone --depth 1 --branch v0.28.0 https://github.com/y-crdt/y-crdt.git .deps/y-crdt-wasm
patch -d .deps/y-crdt-wasm -p1 < examples/shared-text/yrs-0.28-emscripten.patch
export CARGO_HOME="$PWD/.deps/cargo-reference"
export CARGO_TARGET_DIR="$PWD/.deps/y-crdt-wasm-target" CARGO_BUILD_JOBS=1 EMCC_CORES=1
nice -n 15 ionice -c 3 cargo build --manifest-path .deps/y-crdt-wasm/Cargo.toml \
  --package yffi --locked --jobs 1 --target wasm32-unknown-emscripten

sh "$HOME/Qt/6.11.2/wasm_singlethread/bin/qt-cmake" -S . -B build-qt-wasm -G Ninja \
  -DQT_HOST_PATH="$HOME/Qt/6.11.2/gcc_64" -DBUILD_TESTING=OFF \
  -DP2PDC_YRS_INCLUDE_DIR="$PWD/.deps/y-crdt-wasm/tests-ffi/include" \
  -DP2PDC_YRS_LIBRARY="$PWD/.deps/y-crdt-wasm-target/wasm32-unknown-emscripten/debug/libyrs.a" \
  -DSODIUM_INCLUDE_DIR="$PWD/.deps/sodium-wasm/include" \
  -DSODIUM_LIBRARY="$PWD/.deps/sodium-wasm/lib/libsodium.a"
nice -n 15 ionice -c 3 cmake --build build-qt-wasm --parallel 1 --target p2pdc_shared_text
```

Run `./bin/p2pdc_shared_text_browser` to serve an existing WebAssembly build
on loopback and open it in the browser. Keep that terminal open; Ctrl+C stops
the server. Alternatively serve `build-qt-wasm/examples/shared-text/` from
HTTPS or loopback. Chromium
and Firefox startup and local editing have been checked. Two Chromium peers
have synchronized text over a direct local connection with a disposable
two-node DHT. Three Chromium instances have also joined and shared an edit
over that local network. Internet paths, Firefox collaboration, and the other target
platforms have not yet been validated for this example.

## Android debug APK

The arm64 debug app has been built and tested on a Pixel 6a. The following
recipe uses the Android dependency prefixes already prepared in this workspace;
see [Android dependencies](../../docs/ANDROID.md) for their build recipes. It
requires the Qt 6.11.2 desktop and Android kits, Android NDK 27.2.12479018,
JDK 21, the Android SDK, and Rust's `aarch64-linux-android` target. The native
networking dependencies used for the test were built with NDK 27.0; a fully
fresh dependency build and Android lifecycle testing are still outstanding.
On this machine the packaging tools selected compile SDK 37 and warned that
the Android Gradle plugin was only tested through 36.1; the debug APK built
and ran on the test phone despite that warning.

From the library root, first build Yrs for Android:

```sh
CARGO_HOME="$PWD/.deps/cargo-reference" \
CARGO_TARGET_DIR="$PWD/.deps/y-crdt-android-target" CARGO_BUILD_JOBS=1 \
CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=/opt/android-sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
  nice -n 15 ionice -c 3 cargo build --manifest-path .deps/y-crdt/Cargo.toml \
  --package yffi --locked --jobs 1 --target aarch64-linux-android
```

Configure the app and its target-only dependencies:

```sh
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
export ANDROID_SDK_ROOT=/opt/android-sdk ANDROID_NDK_ROOT=/opt/android-sdk/ndk/27.2.12479018
export PKG_CONFIG_LIBDIR="$PWD/.deps/android-nice/lib/pkgconfig:$PWD/.deps/android-glib/lib/pkgconfig:$PWD/.deps/android-opendht/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
nice -n 15 ionice -c 3 "$HOME/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake" \
  -S . -B build-android-shared-text -G Ninja -DQT_HOST_PATH="$HOME/Qt/6.11.2/gcc_64" \
  -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="$PWD/cmake/AndroidStaticDependencies.cmake" \
  -DP2PDC_ANDROID_DATACHANNEL_PREFIX="$PWD/.deps/android-datachannel" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DCMAKE_FIND_ROOT_PATH="$PWD/.deps/android-nice;$PWD/.deps/android-glib;$PWD/.deps/android-opendht" \
  -DCMAKE_PREFIX_PATH="$PWD/.deps/android-nice;$PWD/.deps/android-glib;$PWD/.deps/android-opendht" \
  -DPKG_CONFIG_ARGN=--static -DBUILD_TESTING=OFF -DP2PDC_BUILD_SHARED_TEXT=ON \
  -DSODIUM_INCLUDE_DIR="$PWD/.deps/sodium-android-arm64/include" \
  -DSODIUM_LIBRARY="$PWD/.deps/sodium-android-arm64/lib/libsodium.a" \
  -DP2PDC_YRS_INCLUDE_DIR="$PWD/.deps/y-crdt/tests-ffi/include" \
  -DP2PDC_YRS_LIBRARY="$PWD/.deps/y-crdt-android-target/aarch64-linux-android/debug/libyrs.a" \
  -DCMAKE_SHARED_LINKER_FLAGS='-Wl,--threads=1'
```

Build and package with one job. The debug signing key and Gradle cache stay in
this workspace; the Java options keep the Gradle worker memory bounded:

```sh
mkdir -p .deps/android-user/.android
JAVA_TOOL_OPTIONS="-Duser.home=$PWD/.deps/android-user -XX:ActiveProcessorCount=2" \
GRADLE_USER_HOME="$PWD/.deps/gradle-android-app" \
GRADLE_OPTS='-Dorg.gradle.daemon=false -Dorg.gradle.workers.max=1 -Dorg.gradle.jvmargs=-Xmx768m' \
  nice -n 15 ionice -c 3 cmake --build build-android-shared-text \
  --target p2pdc_shared_text_make_apk --parallel 1
```

The APK is at
`build-android-shared-text/examples/shared-text/android-build/build/outputs/apk/debug/android-build-debug.apk`.
Connect an authorized phone and install it with `adb install PATH_TO_APK`; use
`adb install -r PATH_TO_APK` to update this same debug app. This is an in-memory
example, not a durable document app or a production release.

## Connect participants

1. Generate one capability in either instance and share it privately with the
   others. Enter the same capability in every instance.
2. Configure an OpenDHT bootstrap host for native peers and an HTTPS OpenDHT
   proxy for browser peers. The initial values use Jami's public infrastructure;
   you can replace them with your own. Supply STUN and TURN settings if the
   network requires them. For TURN/TLS, enter the TLS server port explicitly.
3. Start each instance and select **Join**. Peers discover and connect to one
   another automatically. The status shows the number of connected peers and
   whether a direct route or TURN relay was observed. **Leave** keeps the local
   document in memory; reconnecting merges changes made while disconnected.

The capability is a secret, not a password to publish. Anyone who obtains it can
join and edit; this example does not independently verify participant identities.
Each peer attempts a connection to every other peer, so the example is intended
for small groups. The default node limit is eight simultaneous connection
handles per participant. Complete document history is also bounded by the
negotiated message limit. A separate bounded replay cache can temporarily
prevent joining after too many recent offers.
The application should be stopped before closing the window. Qt's platform and
browser environments may expose the fields and memory to the local user.

## Tests

With `BUILD_TESTING=ON`, build `p2pdc_shared_text_tests`,
`p2pdc_shared_text_ui_tests`, and `p2pdc_shared_text_session_tests`. The latter uses a disposable local OpenDHT
node and establishes real local p2pdc connections. The Linux isolated test
needs `unshare` and `ip` and uses an unprivileged network namespace:

```sh
nice -n 15 ionice -c 3 cmake --build build-native --parallel 1 \
  --target p2pdc_shared_text_tests p2pdc_shared_text_ui_tests p2pdc_shared_text_session_tests p2pdc_shared_text_qmllint
nice -n 15 ionice -c 3 ./bin/p2pdc_shared_text_tests
nice -n 15 ionice -c 3 ./bin/p2pdc_shared_text_ui_tests
nice -n 15 ionice -c 3 bash examples/shared-text/run-isolated-tests.sh
```

If your test binary is in another build directory, set
`P2PDC_SHARED_TEXT_TEST_BINARY` to its absolute path.
