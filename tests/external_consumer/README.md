# External CMake consumer check

This fixture uses the public target from a parent project, including a parent
Ninja job pool. The same C++ source creates a node with a valid identity and
checks that an invalid identity is rejected. The browser page also checks that
both internal JavaScript bridges were linked. It does not establish a network
connection; the standalone echo checks cover that separately.

From the library root, after installing the native dependencies described in
the main README and building an Emscripten-targeted libsodium:

```sh
cmake -S tests/external_consumer -B build-external-native -G Ninja \
  -DBUILD_TESTING=OFF -DP2PDC_BUILD_SHARED_TEXT=OFF -DP2PDC_SOURCE_DIR="$PWD" \
  -DLibDataChannel_DIR="$PWD/.deps/install/lib/cmake/LibDataChannel"
cmake --build build-external-native --target external_consumer --parallel 1

cmake -S tests/external_consumer -B build-external-wasm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$EMSCRIPTEN_TOOLCHAIN_FILE" \
  -DBUILD_TESTING=OFF -DP2PDC_BUILD_SHARED_TEXT=OFF -DP2PDC_SOURCE_DIR="$PWD" \
  -DSODIUM_INCLUDE_DIR="$P2PDC_WASM_SODIUM/include" \
  -DSODIUM_LIBRARY="$P2PDC_WASM_SODIUM/lib/libsodium.a"
cmake --build build-external-wasm --target external_consumer --parallel 1
bash tests/run_external_consumer_smoke.sh
```

Set `EMSCRIPTEN_TOOLCHAIN_FILE` and `P2PDC_WASM_SODIUM` to the installed
toolchain file and WebAssembly libsodium prefix. The smoke script needs Linux
user/network namespaces, Chromium, Python with `websocket-client`, `curl`,
`ip`, and `setsid`. It launches an isolated loopback web server and browser and
cleans them up afterward. It uses fixed test secrets, never real identities.
