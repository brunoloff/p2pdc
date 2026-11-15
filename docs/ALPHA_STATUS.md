# First alpha status

Status as of 2026-11-15: the library is an experimental implementation; the
first alpha is prepared locally and has not been published. Its targets are
Linux and browser WebAssembly in tested Chromium and Firefox versions; Android
work resumes tomorrow. Safari and mobile-browser support are not yet tested.
The public C++ consumer interface is
shared across native and WebAssembly builds; Yrs is only an optional reference
workload and is not a library dependency.

## Evidence collected

- A clean Linux build with the pinned patches and host dependencies passed 534
  registered CTest tests. Independent Python vector checks are included; the
  encrypted-record checks require optional PyNaCl.
- Controlled isolated native and Chromium tests covered TURN relay, automatic
  fallback with direct checks blocked, and rejected credentials. Route reporting
  was checked at the message exchange, including local and remote relay cases.
- Patched Linux TURN/TLS peers exchanged authenticated messages through an
  isolated relay. Controls rejected untrusted and mismatched certificates,
  expired credentials, an unavailable server, and a plaintext-only server.
  Automatic mode selected the TLS relay when direct traffic was blocked.
- Chromium and Firefox passed controlled TURN/TLS tests for trusted and
  untrusted CAs, hostname mismatch, and expired credentials. The tests use
  disposable certificate stores and do not modify the host trust store.
- Chromium and Firefox public C++ browser connections exchanged messages with
  native peers. Android and browser peers exchanged messages across different
  networks, including automatic phone-side TURN fallback with a blocked-direct
  control. These are particular runs, not a reliability claim for all networks.
- Both browsers passed direct and automatic TURN-fallback tests and checked
  that a missing WebRTC API returns `Unsupported`, including preservation of a
  pending incoming request.
- A freshly rebuilt Android dependency chain linked the public echo. It passed
  direct and TURN-fallback exchanges between a Pixel 6a and Firefox; details and
  build limitations are in [Android testing](ANDROID.md).
- The optional [Yrs reference workload](../integrations/yrs/README.md) passed
  offline edits and catch-up after a new native connection. It does not test a
  Yjs/browser peer.

## Remaining before a supported alpha

The authoritative release scope and post-alpha deferrals are tracked in
[TODO.md](../TODO.md). In particular, protocol freeze, expanded adversarial and
network testing, relay-only privacy verification, and disposable messages are
deferred until after the first alpha.

- Safari and mobile-browser validation is deferred until after this alpha.
- The same C++ consumer passed simultaneous-dialing and remote
  rejection/cancellation tests on Linux and in Chromium and Firefox WebAssembly.
  Mixed native/browser races also pass in both engines. Alpha uses documented
  bounded signaling records instead of chunking.
- Native TURN/TLS requires the patched libnice backend; an unpatched backend is
  rejected. Errors without a specific backend cause remain generic, and
  relay-only privacy verification is deferred.
- Prepare a reviewable release history and distribution. The library's local
  Git repository currently has no commits or configured remote; no release tag
  or package has been created.

Do not treat test success as proof of production readiness or continuous
availability when the browser or mobile operating system suspends an app.
