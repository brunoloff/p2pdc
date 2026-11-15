# Run the actual security tests in WebAssembly

From the library root:

```sh
bash examples/wasm-security/run.sh
```

The script builds libsodium and GoogleTest into WebAssembly, then compiles the
actual, unchanged security, recipient encryption, advertisement, discovery-key, and signaling
sources and tests. Node runs the resulting executable. This exercises the same RFC 8032 Ed25519
signature vector and independent fixed XChaCha20-Poly1305 decryption vector as
the native tests, including authentication rejection and the other security tests.
No test vectors or assertions are copied into a separate harness.

## Requirements and pinned inputs

- Linux with Bash, Git, Make, Node, Python 3, `mktemp`, `sha256sum`, and `tee`.
- A working Emscripten toolchain at `/usr/lib/emscripten`, including its configured
  Clang, wasm linker, and Binaryen tools. Override its directory with
  `P2PDC_EMSCRIPTEN_ROOT` if necessary.
- Outbound HTTPS to GitHub for both dependency clones, and writable `/tmp`.
- libsodium `1.0.22-RELEASE`, commit
  `77e1ce5d6dee871c49ef211222ba18ef0c486bda`.
- GoogleTest `v1.17.0`, commit
  `52eb8108c5bdec04579160ae17225d66034bd723`.

Both cloned commits are checked before building. The release checkouts provide
configured build scripts, so Autoconf regeneration is unnecessary. libsodium is
built as portable static C with assembly and pthreads disabled. C++ exceptions
are enabled for the actual C++ tests; GoogleTest's pthread support is disabled.

The tested environment was Emscripten `6.0.10-git`
(`d6c521a7f05449857c76bd99e396895583cf2083`) and Node `v24.6.0`.
Other toolchain versions are not pinned by the script and require verification.
GoogleTest produces an upstream `char8_t` conversion warning on this toolchain;
the build completes without changing or suppressing the tests.

## Outputs and failure handling

Each invocation creates a fresh `/tmp/p2pdc-wasm-security.XXXXXX` directory and
preserves it, including failed builds. It never deletes existing files or writes
project sources. Emscripten's shared cache is explicitly
`EM_CACHE=/tmp/p2pdc-emcache`; avoid concurrent first-time cache builds if your
Emscripten installation does not support locking.

The first output line names the build directory. Inspect `sodium-configure.log`,
`sodium-build.log`, `test-build.log`, and `test-results.log` there. Generated
`security-test.js`, `security-test.wasm`, dependency sources, archives, version
receipts, and source hashes all remain outside the repository. Source hashes
are checked before executing the tests, so concurrent edits stop the run rather
than silently mixing source versions. A failure exits nonzero. Missing tools
and dependency commit mismatches exit 2.

The expanded suite covers fixed vectors, seed and recipient-key import/export,
binary and empty payloads, wrong-key and associated-data rejection, tampering,
capability binding, malformed lengths, and resource bounds.

## Scope of the result

This is a Node-hosted WebAssembly test. It establishes that the project's native
crypto code and its exact vectors execute under this Wasm toolchain. It does not
exercise Chromium, Firefox, Safari, browser loading, HTTPS/secure-context random
sources, browser persistence, WebRTC, or OpenDHT proxy interoperability. The
executable is linked with `-sENVIRONMENT=node`; a browser harness needs a separate
build and test.

Node supplies cryptographic randomness through Emscripten's runtime. A passing
freshness test is functional evidence, not an independent entropy audit. Wasm's
linear memory also does not provide native guard-page or locked-memory
protection for `sodium_malloc`; these tests verify functional secret allocation,
zeroing APIs and crypto behavior, not isolation from the JavaScript host.

## Script checks

```sh
bash -n examples/wasm-security/run.sh
shellcheck examples/wasm-security/run.sh
P2PDC_EMSCRIPTEN_ROOT=/tmp/nonexistent-emscripten \
  bash examples/wasm-security/run.sh
# Expected: missing-tool diagnostic and exit 2, before creating a build directory.
```
