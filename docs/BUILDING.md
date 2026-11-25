# Building p2pdc

These steps start from a clean checkout. Dependencies are downloaded at pinned versions into
`build/`; no third-party source is stored in this repository. GitHub Actions runs the same
commands for every push, with one workflow per platform in `.github/workflows/`: Ubuntu 24.04
(`linux.yml`), browsers (`web.yml`), macOS on Apple Silicon and Intel (`macos.yml`, presets
`deps-macos` and `macos`) and Windows in an MSYS2 UCRT64 shell (`windows.yml`, presets
`deps-windows` and `windows`).

## What you install yourself

CMake 3.25 or newer, Ninja, Git, a C++20 compiler, pkg-config, Meson, and the development files
of GLib, OpenSSL, GnuTLS, Nettle, Argon2, fmt and GoogleTest.

On Ubuntu 24.04:

```sh
sudo apt-get install build-essential cmake ninja-build git pkg-config meson libglib2.0-dev \
  libssl-dev libgnutls28-dev nettle-dev libargon2-dev libfmt-dev libgtest-dev libgmock-dev
```

On Arch Linux or Manjaro:

```sh
sudo pacman -S --needed base-devel cmake ninja git pkgconf meson glib2 openssl gnutls nettle \
  argon2 fmt gtest
```

## Linux

```sh
git clone https://github.com/brunoloff/p2pdc.git
cd p2pdc
cmake --workflow --preset deps
cmake --workflow --preset linux
```

The first command builds libsodium, msgpack-cxx, OpenDHT, the patched libnice and
libdatachannel (with WebSocket support), libsecp256k1 and nlohmann/json into
`build/prefix-linux`. It only has to run again when `deps/` or `patches/`
change. The second builds p2pdc and runs its tests on the local machine, without using the
internet.

To keep the computer responsive during builds, limit the work done at once:

```sh
export CMAKE_BUILD_PARALLEL_LEVEL=1
```

## Browsers

```sh
tools/install-emsdk.sh .emsdk
source .emsdk/emsdk_env.sh
cmake --workflow --preset deps-web
cmake --workflow --preset web
```

The first line installs Emscripten 4.0.7 into `.emsdk`. Browsers provide WebRTC themselves, so
libsodium is the only library built for this target.

## Using p2pdc in another project

Add p2pdc with `add_subdirectory()` or CMake's FetchContent and link `p2pdc::p2pdc`. To build
its dependencies from a parent project, configure the p2pdc source with
`-DP2PDC_BUILD_DEPENDENCIES=ON -DCMAKE_INSTALL_PREFIX=<folder>`, build it, and add that folder
to the parent's `CMAKE_PREFIX_PATH`.

Android is built separately; see [ANDROID.md](ANDROID.md).
