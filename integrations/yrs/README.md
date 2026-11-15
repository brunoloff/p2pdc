# Optional Yrs reference workload

This isolated native integration test links Yrs's C interface into a test
consumer. The `p2pdc` library itself has no Yrs dependency or CRDT-specific
API. The test sends opaque Yrs v1 state vectors and updates over public
`Connection` handles, disconnects two replicas, edits both independently,
opens a fresh connection, exchanges differential updates, and checks that the
two texts converge. A separate check rejects malformed and empty updates.

The test uses two Yrs replicas on a local native OpenDHT network. It does not
yet test a Yjs peer, a browser replica, persistence, internet paths, or Yrs
versions other than the pinned source below. It is a reference workload, not a
production synchronization protocol.

From the library root, with the native build prerequisites, Cargo, and Git
installed:

```sh
git clone --depth 1 --branch v0.28.0 https://github.com/y-crdt/y-crdt.git .deps/y-crdt
export CARGO_HOME="$PWD/.deps/cargo-reference"
export CARGO_TARGET_DIR="$PWD/.deps/y-crdt-target"
export CARGO_BUILD_JOBS=1
nice -n 15 ionice -c 3 cargo build --manifest-path .deps/y-crdt/Cargo.toml \
  --package yffi --locked --jobs 1

cmake -S tests/external_consumer -B build-external-native -G Ninja \
  -DBUILD_TESTING=OFF -DP2PDC_SOURCE_DIR="$PWD" \
  -DLibDataChannel_DIR="$PWD/.deps/install/lib/cmake/LibDataChannel" \
  -DP2PDC_YRS_INCLUDE_DIR="$PWD/.deps/y-crdt/tests-ffi/include" \
  -DP2PDC_YRS_LIBRARY="$PWD/.deps/y-crdt-target/debug/libyrs.so"
nice -n 15 ionice -c 3 cmake --build build-external-native \
  --target p2pdc_yrs_reference --parallel 1
nice -n 15 ionice -c 3 ./build-external-native/p2pdc_yrs_reference
```

The example commands use Linux paths and the local libdatachannel installation
described in the main README. `libyrs.so` and the header must come from the
same Yrs build. The Rust build emits upstream compiler warnings with the local
toolchain. Test identities are disposable and must not be used in an app.
