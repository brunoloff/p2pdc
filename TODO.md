# Release checklist

The current alpha targets Linux and browser WebAssembly. Android is deferred
until tomorrow. The same
unmodified C++ consumer source must work natively and in WebAssembly. Yrs is
only a reference workload, not a library dependency.

## Alpha

### Connection and signaling

- [x] Resolve simultaneous dialing deterministically so peers converge on one
  connection, including concurrent acceptance and disconnect cases. Tests cover
  native, Chromium, Firefox, mixed Linux/browser peers, and two Android peers
  on one device, using controlled local discovery.
- [x] Notify the remote peer of rejection and cancellation, and report useful
  outcomes rather than relying on timeouts. Tests cover both mixed Linux/browser
  roles and local native, browser and Android pairs. Delivery is best effort.
- [x] Explicitly rule out signaling batching/chunking for alpha; document and
  test the bounded SDP, candidate, and record-count limits.

### Public API and TURN

- [x] Propagate informative failure categories where supported, including
  signaling, protocol/authentication, transport, timeout and resource limits;
  do not infer a specific credential failure from a generic ICE error. Known
  signaling-store, incoming mailbox, native transport, and reliable-channel
  failures retain their categories. Paths without sufficiently specific
  backend evidence continue to report a generic error.
- [x] Implement native TURN-over-TLS with certificate validation and test
  trusted DNS and IP identities, untrusted certificates, identity mismatches,
  inaccessible and plaintext servers, expired credentials, and relay exchange.
  The API rejects TLS if the patched libnice backend is not loaded.
- [x] Validate browser TURN/TLS in the alpha engines. Firefox and Chromium
  passed controlled trusted, untrusted-CA, wrong-hostname, and
  expired-credential tests.
- [x] Defer a credential-provider API: alpha callers supply fresh credentials
  for each attempt, with no automatic refresh during a live attempt.

### Platform and interoperability

- [x] Complete browser coverage for the tested Chromium and Firefox versions.
- [x] Reconfirm the same unmodified C++ consumer source builds and runs natively
  and as WebAssembly for discovery, connection, messaging, route reporting,
  and shutdown.

### Packaging and release

- [x] Prepare pinned dependencies, clean build and consumer instructions,
  platform limitations, protocol version, test commands, reviewable source
  history and a local source-release artifact. Host-provided dependencies are
  recorded by version; the source and tag are pushed to a private repository.
- [x] Run the final release matrix using controlled local infrastructure;
  public-network tests remain opt-in.

## Post-alpha

### High priority: multi-peer foundations

Membership and room policies belong to higher-level code for now. The basic
stack should let peers sharing a capability discover and connect to one another
without requiring a fixed coordinator or prescribing a connection topology.

- [x] Improve incoming connection handling for multiple peers: replace or
  extend the single-shot listener with a persistent interface that can expose
  and independently accept or reject concurrent requests. Define bounded
  pending-request and overload behavior, cancellation, and lifecycle semantics.
- [x] Remove the fixed eight-connection-handle limit, or make the limit
  configurable. Account for concurrent attempts and retained handles, preserve
  explicit resource limits and failure reporting, and consider capacity shared
  across capabilities on the same node.

### Other post-alpha work

- [x] Improve behavior when the per-lease replay cache fills: new offers are
  now dropped while the listener stays open, without evicting replay entries.
- [ ] Validate Safari and mobile browsers, which are outside the first alpha's
  supported browser target.
- [ ] Finish Android support: run the documented build from a fresh checkout and
  exercise backgrounding, screen-off/suspension, resume, network changes, and
  process termination in a real application host. Work resumes tomorrow.
- [ ] Add optional disposable unordered, non-retransmitted messages, including
  negotiation, limits, backpressure, and fairness tests.
- [ ] Verify relay-only privacy, including whether all direct ICE traffic is
  prevented; do not claim this privacy property before verification. Signaled
  candidates and SDP are filtered (audit #3); the network behaviour of the ICE
  backends is still unverified.
- [ ] Review memory and load limits, including backend memory, stalled peers,
  concurrent attempts, sustained load, and recovery under pressure.
- [ ] Decide which operation/connection identifiers and counters to expose.
- [ ] Review and freeze the protocol, including identity binding, replay
  protection, authorization, key handling, and SDP/fingerprint binding.
- [ ] Extend adversarial and sanitizer testing of parsers, authenticated input,
  object lifetimes, and concurrency.
- [ ] Document security boundaries and what discovery, proxies, and TURN can
  observe, including infrastructure requirements.
- [ ] Expand network testing across additional NATs, firewalls, and roles.
- [ ] Add browser proxy failover and recovery across multiple gateways.
