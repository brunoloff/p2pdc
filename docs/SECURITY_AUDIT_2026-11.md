# Security and robustness audit, November 2026

Scope: a read-only review of `src/`, `include/p2pdc/`, `patches/`, the build
files and the protocol documents at commit `7bd9006`. Nothing was built or run
for the audit itself. Each fix below is a separate commit on the `audit-fixes`
branch; its message names the issue identifier used here.

## Summary

No Critical issue was found. The cryptographic design (capability-derived keys
with domain separation, Ed25519-signed and sealed signaling, SDP-bound DTLS
fingerprints, signed hello/confirm transcript over both SDP hashes) and the
parsers (length-checked, overflow-safe) held up. The main weaknesses are
availability: untrusted or insider input can permanently stop discovery,
mailboxes and listeners, because several resource limits are terminal.

## Findings and plan

Severity reflects the impact on the library's documented goals. "Confirmed"
means traced in the code; "speculative" means it needs investigation before a
code change.

### High

**#1 One unauthenticated DHT record permanently stops a watch, mailbox or
listener (confirmed).** OpenDHT values are unsigned. `native_store.cpp`
ends a watch or lookup with `ResourceLimit` when one value is larger than
`max_value_bytes` or a batch exceeds `max_values`/`max_total_bytes`; the mailbox
inbound queue does the same, and `recovering_mailbox.cpp` classifies
`ResourceLimit` as non-retryable. Anyone who learns the 20-byte key (DHT nodes
storing it, proxy observers, any capability holder) can stop discovery on a
`FixedKeyV1` key permanently, or an inbox for a lease's lifetime.
*Plan:* treat oversize records and excess records from the store as untrusted
input to drop and count (`StoreBatch::dropped`), not as a terminal error, in
both the native and browser store adapters. Give the mailbox inbound queue a
drop policy for oversized or excess remote records. Keep terminal errors for
local faults (allocation failure, backend failure). Tests: oversize and excess
records are dropped while the watch and later valid records continue.

### Medium

**#2 A group member can stop a peer's listener or watch by filling bounded
tables (confirmed).** Fresh Ed25519 keys are free. When the per-lease replay
cache (32), the unknown-peer offer table (16) or the advertisement tracker is
full, the listener or watch fails. *Plan:* when a table is full, drop the new
input (and, for offers, keep replay entries intact so replay protection is not
weakened) and keep the listener or watch healthy; entries free up as they
expire. Tests: a full table drops new input without failing and accepts again
after expiry.

**#3 Relay-only mode does not check what it signals (confirmed design gap).**
`RequireLocalRelay` relies on the browser `iceTransportPolicy: "relay"` and on
libnice force-relay; the library itself sends every local candidate and the
local SDP unfiltered. *Plan:* when relay is required, drop local candidates
whose type is not `relay` and strip non-relay `a=candidate` lines from the local
SDP before signing, natively and in the browser. Tests: filter unit tests.

**#4 A backwards wall-clock step is fatal (confirmed).** Leases, the
advertisement tracker, the windowed mailbox and the replay cache fail
permanently when `system_clock` goes backwards, which NTP steps and resume from
suspend can cause. *Plan:* clamp the wall clock to the last observed value at
each site (time does not go backwards; validity checks still use the clamped
time) instead of failing. Tests: a backwards step is tolerated.

**#5 Leases do not recover from outages (confirmed).** Failed puts retry every
pulse without backoff, count towards `max_publications`, and a fixed-key lease
ends after its publication budget. *Plan:* failed puts do not consume the
publication budget, retry with bounded exponential backoff, and the budget is
measured per refresh window. Tests: repeated failures do not exhaust the lease.

**#6 `CallbackGate::post` leaks its pending count if the executor throws
(confirmed).** Shutdown completion is then never delivered. *Plan:* decrement
and notify on throw, as `BackendDrain::submit` does. Test: throwing executor.

**#7 Linkability across rotations and groups (confirmed).** The signaling inbox
key is constant for a lease and is `listen`ed on, and the default node identity
is reused for every lease and capability. *Plan:* without an imported identity,
use a fresh endpoint identity per lease as the README already promises, and
document that the inbox is stable for a lease. Rotating the inbox inside a lease
changes the protocol and is recorded for the protocol freeze.

**#8 libdatachannel patch makes `trySendMessage` throw (speculative).** It also
runs on internal flush and close paths. *Plan:* audit every caller in the pinned
libdatachannel source; if any can let the exception escape a thread or
`noexcept` frame, change the patch so internal paths keep the non-throwing
behaviour.

**#9 libnice TLS patch fails open outside RFC5245 mode (confirmed).**
`NICE_RELAY_TYPE_TURN_TLS` in other compatibility modes creates a plaintext TCP
TURN socket. *Plan:* refuse TURN-TLS in modes the patch does not handle.

**#10 Browser proxy listen streams have no idle timeout or rate limit
(confirmed).** A stalled stream holds one of two stream slots indefinitely, and
a hostile proxy can stream records at any rate. *Plan:* idle timeout that
restarts the stream, a per-stream record rate bound, and tolerate blank
keep-alive lines.

### Low

- **L1 Capability copies are not wiped.** Use a wiping secret container for the
  long-lived capability copies.
- **L2 Pre-handshake reassembly can reserve a full-size message.** Cap the
  reassembly size at the handshake record size until the channel is ready.
- **L3 Callback exceptions are rethrown after the failure is recorded,** which
  terminates the process under the bundled runtime. Record and log instead.
- **L4 No build hardening flags; `-Werror` on library targets.** Add a
  `P2PDC_HARDENING` option (on by default for GCC/Clang) and a `P2PDC_WERROR`
  option (on only for top-level builds).
- **L5 Partial dependency pinning.** Make the Android libnice and OpenDHT
  dependency instructions check out and verify exact commits.
- **L6 `bufferedAmountLowThreshold` is 0,** giving stop-and-wait sending. Use a
  quarter of the send-buffer limit.
- **L7 Duplicated time helpers** in four translation units. Share one.
- **L8 Repository hygiene.** Ignore `aqtinstall.log`. (The survey said `bin/`
  held compiled binaries; it holds launcher scripts and is now tracked.)
- **L9 TURN-TLS context per socket and partial wildcards.** Set
  `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`.
- **L10 Early handshake frame race (speculative).** A frame arriving before the
  gate exists fails the attempt. Investigate; buffer if reachable.
- **L11 Received messages are copied outside the memory budget.** Account for or
  avoid the copy.
- **L12 WebAssembly secret handling limits are undocumented.** Document them.
- **L13 Each inbox record is decrypted twice while offers are pending.** Decrypt
  once.
- **L14 The remote peer is left waiting** when local setup fails after an offer
  was consumed. Send a reject.
- **L15 Overflow and close rejections are unthrottled.** Bound them per sender.
- **L16 An inline executor deadlocks the mailbox.** Do not schedule while
  holding the submission mutex.
- **L17 `join_until_stopped` can spin forever.** Bound the retries.
- **L18 The capability is compared with `operator==`.** Use `sodium_memcmp`.
- **L19 Proxy lookup inspection is unbounded within the timeout.** Bound the
  number of inspected lines.
- **L20 ICE host validation accepts `;`, `%` and `,`.** Reject them.
- **L21 An invalid browser transport state closes silently.** Report an error.
- **L22 Spec and code differ** (`HANDSHAKE_V1.md`, `FRAMING_V1.md`). Update the
  documents.
- **L23 The libnice patch now resolves IP literals asynchronously
  (speculative).** Investigate.
- **L24 (found while testing) The shared-text example breaks embedded
  builds.** `P2PDC_BUILD_SHARED_TEXT` defaulted to ON even when p2pdc was
  embedded, so `p2pdc_opendht_embedded_target` failed to configure.

### Design suggestions

- **S1** `Identity::sign` signs arbitrary bytes. Prefix application signatures
  with a library-owned domain so they can never be valid protocol records.
- **S2** Bind the handshake to the DTLS exporter natively. Browsers do not
  expose the exporter, so this cannot be symmetric; recorded for the protocol
  freeze rather than changed now.
- **S3** `HandshakeExchange` stores `const Identity&`. Hold shared ownership.
- **S4** Expose dropped-record diagnostics (partly delivered by #1's counters).
- **S5** Deprecate `FixedKeyV1` in documentation in favour of rotating keys.

## Verification

Each fix is built with one job at low priority in `build-native` and its
targeted tests are run serially. Browser (`.mjs`) changes are checked with the
existing Node-based tests where they exist.

## Resolution

Each item was fixed in its own commit on `audit-fixes`, in priority order,
except where noted. "No change" means the investigation showed the code was
already correct; the commit or note records why.

| Item | Outcome | Commit |
| --- | --- | --- |
| #1 | Over-limit records are dropped and counted; watches, mailboxes and listeners stay open (native, browser C++ and JS proxy). | `85b22ff` |
| #2 | Full replay history drops new offers (no early eviction); unknown-peer table evicts oldest; full tracker ignores new peers. The public pending-event queue still fails on overflow, deliberately. | `1ca2ef8` |
| #3 | Relay-only mode filters signaled candidates and SDP. Also fixed a confirmed leak: libnice put the LAN address in relay candidates' `raddr`. | `cef2f1e` |
| #4 | Backward steps up to 30 s are absorbed; larger ones still fail and recreated components work. | `f483dc7`, `3c588c9` |
| #5 | Exponential retry backoff, expired pending records republished, publication budget per 120 s window in both protocols. | `ff239e1` |
| #6 | `CallbackGate::post` releases its slot when the executor throws. | `5b6b2d3` |
| #7 | Opt-in `AnnounceOptions::fresh_identity`; default kept because `Node::identity()` is public API. Per-lease inbox rotation is a protocol change left for the freeze. | `617161d` |
| #8 | No change: every internal caller of the throwing send catches; audit recorded in DEPENDENCIES.md. | `693e1ea` |
| #9 | Patched libnice refuses TURN-TLS in modes without TLS. | `2a669d0` |
| #10 | Idle listen streams restart transparently; per-stream record rate cap; blank lines tolerated. | `42677aa` |
| L1 | Capability copies wiped on release. | `5348bd8` |
| L2 | Inbound messages capped at one control record until the handshake completes. | `4828020` |
| L3 | Callback exceptions logged, not rethrown. | `9c1c497` |
| L4 | `P2PDC_HARDENING` (default on) and `P2PDC_WERROR` (top-level default). | `c8b90b9` |
| L5 | Android clones verified by commit; `patches/SHA256SUMS`. | `45fb575` (checksums updated in `57aaa3d`) |
| L6 | Writable threshold at a quarter of the buffer limit, native and browser. | `5b6bc16` |
| L7 | Duplicated time helpers replaced by `src/core/wall_clock`. | `f483dc7` |
| L8 | `aqtinstall.log` ignored; `bin/` was a false finding (launcher scripts). | `aaf52bc` |
| L9 | Shared TURN-TLS `SSL_CTX`; no partial wildcards. | `57aaa3d` |
| L10 | No change: not reachable for honest peers; reasoning recorded in code comments. | `caacce1` |
| L11 | Application messages delivered without a second copy. | `763de7e` |
| L12 | WebAssembly secret-handling limits documented. | `4a30e90` |
| L13 | Cancel checks skip records whose size cannot be a cancel. | `7fbf933` |
| L14 | Consumed requests rejected when accepting fails; single-shot behaviour documented. | `c0f5196` |
| L15 | Overflow rejections limited to 8 per 10 s per listener. | `7e27812` |
| L16 | Inline executors detected and rejected instead of deadlocking. | `bebebbb` |
| L17 | Join retries back off to 5 s and log once (still retried; returning early is unsafe). | `5958e4e` |
| L18 | Capability compared with `sodium_memcmp`. | `4e3389f` |
| L19 | Proxy lookups stop reading after the inspection bound. | `9cd89ee` |
| L20 | Host names restricted to host-name characters. | `f709bed` |
| L21 | Invalid bridge events reported as transport errors. | `45e12c3` |
| L22 | Handshake and framing specs aligned with the code. | `3104941` |
| L23 | No change: resolution is decided by `nice_address_is_valid(&turn->server)`, not by `server_address`, so IP literals are not resolved asynchronously. | none |
| L24 | Example defaults to ON only at top level; embedded test uses a clean directory. | `fdf03f7` |
| S1 | Public signatures carry `application_signature_domain`; protocol code uses internal raw signing. | `7b2adfa` |
| S2 | Deferred: browsers do not expose the DTLS exporter, so binding cannot be symmetric. Protocol-freeze item in TODO.md. | none |
| S3 | Identity shared through the handshake chain. | `cd12029` |
| S4 | Deferred: internal drop counters exist (`StoreBatch::dropped`, `InboundQueue::dropped()`); exposing them publicly needs plumbing through six native layers and the browser path. | none |
| S5 | `FixedKeyV1` documented as deprecated. | `243aa93` |

### Behaviour changes for applications

- Over-limit DHT records, full replay/peer tables and small clock regressions
  no longer end watches, leases or listeners.
- Leases survive outages instead of failing with an expired pending record.
- `Identity::sign` / `verify_signature` now apply an application prefix.
- `HandshakeExchange` takes `std::shared_ptr<const Identity>`.
- ICE hosts accept only host-name characters (no IPv6 zone identifiers).
- Executors that run tasks inline are rejected with `std::logic_error`.
- `-Werror` and the shared-text example default to ON only at top level.

### Final verification

The complete native suite in `build-native` (596 tests, hardening flags on)
passed serially, and every JS unit test file passed except the two
`*_live_test.mjs` files, which require a running OpenDHT proxy.

### Not verified

- Browser smokes that need Chromium in a network namespace were built but not
  run (affects the L21 smoke check).
- Relay-only behaviour on real networks remains unverified (TODO.md).
- Android builds were not rebuilt.
- One intermittent failure of `ExchangesLargeFragmentedMessagesWithinConfiguredLimits`
  was seen once and did not reproduce in later runs.
