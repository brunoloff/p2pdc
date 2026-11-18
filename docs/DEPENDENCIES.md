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
| Emscripten | 4.0.7 for builds (see `tools/install-emsdk.sh`); the original alpha browser checks used 6.0.10-git, commit `d6c521a7f05449857c76bd99e396895583cf2083` |
| Chromium | 157.0.8103.38 |
| Playwright Firefox | 155.0, Playwright browser revision 1543 |

[`deps/CMakeLists.txt`](../deps/CMakeLists.txt) fetches libsodium, msgpack-cxx, OpenDHT, libnice
and libdatachannel at these versions and applies the patches; see
[Building p2pdc](BUILDING.md). The Android OpenDHT and
Jami contrib checkouts, and the Android libnice and GLib clones are verified
against the commits above (the shallow Android clones with a `test` that fails
on a mismatch). `patches/SHA256SUMS` records the patch files; check it with
`(cd patches && sha256sum -c SHA256SUMS)` before applying them. GLib, OpenSSL,
GnuTLS and the other system packages in the build guide are not pinned; the table records
the versions that were tested. The [WebAssembly instructions](WEBASSEMBLY_INTERNALS.md)
pin the libsodium source archive by SHA-256. Other versions have not been
established by these checks; the list is not a claim that older or newer
versions necessarily fail.

## libdatachannel patch: send failures throw

The libdatachannel patch makes a message that SCTP will never deliver (not
connected, send queue stopped, or disconnected mid-send) throw instead of
returning `false`, so p2pdc can tell "accepted but buffered" from "lost".
`SctpTransport::trySendMessage` also runs on internal paths. At commit
`9e6a13ab` every internal caller that can reach the throw catches it:
`SctpTransport::doRecv` (the ACK sent by `IncomingDataChannel::processOpenMessage`
and any other send triggered while processing received data), `doFlush`,
`flush` (used by the SENDER_DRY notification), and
`PeerConnection::iterateDataChannels` (which wraps `DataChannel::open`). The
remaining callers are application-facing (`DataChannel::send`, and
`createDataChannel` when SCTP is already connected) and propagate to p2pdc,
whose `NativeTransport::send` catches the exception and fails the transport.
Re-check these call sites when updating libdatachannel.

## libdatachannel patch: receive callbacks during teardown

libnice may run a receive callback it picked up just before another thread detached it. The
legacy `nice_agent_attach_recv` wrapper frees its callback data during that detach, which crashed
connections closing while data arrived. The patch attaches through
`nice_agent_attach_recv_ex` with its own guard object: the transport waits for a running callback,
marks the guard closed, and the main loop frees it after any callback already under way.
