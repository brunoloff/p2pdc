# 0.1.0-alpha.1

This source alpha provides OpenDHT discovery and authenticated, reliable,
ordered binary WebRTC connections through the same C++ API on Linux and in
browser WebAssembly. It reports observed direct or TURN-relayed routes.

The supported alpha targets are Linux and the tested Chromium and Firefox
versions in [dependency versions](docs/DEPENDENCIES.md). Android, Safari,
other mobile browsers, Windows, and macOS have not completed alpha validation.
The protocol and API may change in a later release.

Applications supply their own OpenDHT bootstrap peers, one reachable HTTPS
OpenDHT proxy for browsers, and STUN/TURN servers and credentials when needed.
TURN-over-TLS on Linux requires the supplied patched libnice and libdatachannel
builds. The browser proxy does not fail over to a second proxy in this alpha.

See the [build instructions](README.md#native-build-and-tests),
[WebAssembly instructions](docs/WEBASSEMBLY_INTERNALS.md),
[browser tests](docs/BROWSER_TESTING.md), and [release checklist](TODO.md).
The source release is licensed under GPL-3.0-or-later; dependencies retain their
own licenses. This alpha is not intended for production use.
