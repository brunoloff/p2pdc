# OpenDHT announcement replacement investigation

The native advertisement lease currently uses a new DHT value for each
publication and has a bounded publication count. Do not remove that bound on
the basis of these experiments: an indefinitely refreshed announcement needs
a safe retention strategy on the actual network.

With installed OpenDHT 4.4.1, a local two-node test successfully published two
RSA-signed values with the same key and ID, and retrieved the newer payload.
A separate direct test of the installed secure edit policy accepted a newer
value signed by the original owner and rejected a conflicting sequence, a
different signer, and a change to an unsigned value. These checks do not prove
replica convergence, public network behavior, or resistance to ID squatting.

Important limitations found during investigation:

- The installed library's signature verification uses RSA-SHA512. The test's
  EC identities produced signed values that were not returned by secure lookup;
  RSA identities passed an explicit local signature check. This is separate
  from the library's existing application-level signing identity.
- A lookup collects responses from multiple nodes and can return different
  payloads with the same value ID. Local publication and remotely received
  replacement requests do not necessarily follow the same storage path.
- A test that submitted conflicting replacements, including an unsigned value
  at a previously signed ID, caused an assertion failure in the installed
  OpenDHT network handler. The upstream secure edit policy dereferences the new
  owner before checking whether the new value is signed. This is a potential
  remotely triggerable crash and must be resolved for any node that uses that
  policy. The crash test was removed from the ordinary suite; the exact
  triggering input still needs an isolated, controlled reproduction.
- OpenDHT's replacement sequence is 16-bit. Its exhaustion, identity
  persistence, hostile writes, and old value expiry need explicit handling.

Reference: [OpenDHT secure edit policy](https://github.com/savoirfairelinux/opendht/blob/master/src/securedht.cpp).
The linked upstream source can change; the observations above describe the
installed version and the local tests, not a guarantee about other deployments.

## Discovery capacity and a possible alternative

The installed OpenDHT headers specify a ten-minute default expiry for user-data
values. Remote nodes apply their own policies, and the browser proxy interface
used here does not set a value type or lifetime. We therefore cannot rely on the
local default to bound public-network retention. In addition, a native watch
previously limited catch-up to 64 raw values before authenticating or discarding
expired advertisements. Several normally refreshing peers could fill that
limit even when the number of actual peers was small.

Discovery catch-up now collapses byte-identical publications even when OpenDHT
assigned different IDs, and bounds the number inspected to eight times the
retained-record limit. This helps when a retry republishes the same bytes. It
does not collapse fresh advertisements, which have new encrypted bytes. Live
discovery also skips identical copies within a native watch batch and while
records wait in either platform's queue; its resource limits still apply.

A candidate alternative is to rotate the discovery lookup key by a short,
fixed time window, derived in the shared C++ code from the capability and a
versioned window number. A watcher would overlap the keys that can contain an
advertisement it would accept, opening and catching up a new key before retiring
an old one. All keys must feed the same peer tracker so that sequence and replay
state survive rotation. Retries must retain the original key and bytes, and
each key still needs a publication budget. Long sleeps, clock skew, and the
start of Unix time need explicit handling. For example, with 120-second
windows, a maximum advertisement lifetime of 300 seconds, and 30 seconds of
accepted skew, the accepted issue-time range spans at most four windows. The
window width is fixed at 120 seconds. The browser accepts a maximum lifetime
of 300 seconds and 30 seconds of skew; native rotating watches reject time
policies that can span more than eight windows.

The shared C++ window-key derivation and browser bridge now exist and have a
cross-platform test vector. The native lease has an experimental rotating-key
mode, and the native listener can follow overlapping windows with one tracker.
The browser lease and WebAssembly codec also have an internal rotating-key
mode, preserving announcement sequence numbers across windows. An internal
browser listener now follows overlapping windows using one tracker and reports
availability for the complete set. Native and browser public nodes default to
the rotating protocol; fixed v1 remains an explicit compatibility option, and
local mismatched watch/announcement protocols are rejected. The native public watcher allows four raw records per
configured peer, with a minimum of 64 and maximum of 1024. Its queue and store
share these bounds and the existing memory budget. A local test catches up with
80 peers, but retries, old announcements, concurrent windows or a smaller memory
budget can still exhaust capacity. Browser proxy lookups and discovery queues
now default to 512 records; recovery delivers buffered records in batches. A
Chromium test exercised 100 peers and 200 announcements using the real
WebAssembly tracker and an in-memory proxy. These are bounded limits, not a
guarantee of discovery under arbitrary traffic. Existing fixed-key peers will
not automatically interoperate with a rotated-key protocol.

An isolated local test now exercises the rotating native listener and lease
against the rotating browser listener and lease through the actual OpenDHT HTTP
proxy. Both peers discover each other at a controlled time, advance across a
window boundary, and observe each other's next sequence. This does not test
internet reachability, a public proxy, or a large peer population. To reproduce
it, build the `p2pdc_browser_windowed_dht_peer` and `p2pdc_browser_secure`
targets, start a fresh `dhtnode` on network 873198, DHT port 49411 and proxy
port 49412, and run `tests/browser_interop_server.py` with the native helper.
Open `tests/browser_windowed_native_smoke.html` on the server's printed port;
stop the server and node after the result. Test identities are fixed.
