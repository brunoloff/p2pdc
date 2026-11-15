# Alpha dependency versions

The Linux and browser checks in this tree used these versions. The two patched
WebRTC dependencies are pinned by commit in the build instructions; their
submodules are pinned by those commits. The remaining Linux dependencies are
provided by the host distribution and are recorded here for reproducibility;
they are not vendored or automatically locked by the build.

| Component | Version used |
| --- | --- |
| libnice | 0.1.24, commit `af62014d520ac09c8fe1974bda1b253114c68843`, with the supplied TLS patch |
| libdatachannel | 0.23.2, commit `9e6a13abbb6846c003d817d0387b6706466e2b03`, with the supplied patch |
| OpenDHT | 4.4.1 |
| msgpack-cxx | 8.0.0 |
| libsodium | 1.0.22 |
| GLib | 2.88.3 |
| OpenSSL | 3.6.5 |
| Emscripten | 6.0.10-git, commit `d6c521a7f05449857c76bd99e396895583cf2083` |
| Chromium | 157.0.8103.38 |
| Playwright Firefox | 155.0, Playwright browser revision 1543 |

The [Linux build](../README.md#native-build-and-tests) instructions fetch and
patch libnice and libdatachannel. The [WebAssembly instructions](WEBASSEMBLY_INTERNALS.md)
pin the libsodium source archive by SHA-256. Other versions have not been
established by these checks; the list is not a claim that older or newer
versions necessarily fail.
