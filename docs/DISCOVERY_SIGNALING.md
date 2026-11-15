# Discovery and signaling formats

These are the current provisional formats, not a frozen interoperability or
security specification. They describe the records stored through OpenDHT or
its HTTPS proxy. Possession of the capability permits discovering and reading
group advertisements; a verified signing key still requires application-level
authorization. Protect the 32-byte capability as secret bearer material.

All integer fields below are unsigned and big-endian. Domain strings are their
ASCII bytes with no terminating NUL. BLAKE2b output lengths are part of the
construction; a 20-byte digest is not a truncated 32-byte digest.

## Discovery keys

The legacy fixed key is BLAKE2b-160 keyed by the capability, over
`p2pdc/discovery/lookup-key/v1`. The default rotating key is BLAKE2b-160 keyed
by the capability, over `p2pdc/discovery/window-key/v2` followed by the
eight-byte window number. The window number is Unix time in seconds divided
by 120, rounded down. Fixed and rotating keys are different namespaces.

For the **test-only** capability `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f`:

| Key | Window | Expected 20-byte digest |
| --- | --- | --- |
| Fixed | — | `ea76a9b4ad1dfbe793c5eae8d96bb51e41b65732` |
| Rotating | 0 | `11c260c6036358b9a09c07042149d9da73d1f953` |
| Rotating | 123456789 | `bab92718c51c00d6b6d92eb52910ffc0cc9a5618` |

The C++ discovery-key tests and `python3 -m unittest tests/discovery_vectors_test.py`
check these values independently. Never use the test capability in an app.

## Advertisement

An advertisement begins with `PDAD` and byte `01`, a 24-byte nonce, then an
XChaCha20-Poly1305 ciphertext and 16-byte tag. The encryption key is BLAKE2b-256
keyed by the capability, over `p2pdc/capability-encryption/key/v1`. Associated
data is `p2pdc/advertisement/aead/v1` followed by the five-byte outer header.
The 197-byte plaintext is the following 133-byte body followed by a 64-byte
Ed25519 signature:

| Body offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Version `01` |
| 1 | 32 | Sender Ed25519 public key |
| 33 | 32 | Sender X25519 recipient public key |
| 65 | 16 | Instance identifier |
| 81 | 20 | Signaling inbox key |
| 101 | 8 each | Sequence, features, issued time, expiry time |

The signature input is `p2pdc/advertisement/signature/v1`, followed by a
32-byte group context and the body. The group context is BLAKE2b-256 keyed by
the capability, over `p2pdc/advertisement/group-binding/v1`. Times are UTC
seconds; validation enforces lifetime, clock-skew and expiry policy. Replay,
feature policy, and authorization are handled above the decoder.

The [advertisement vector](../tests/advertisement_vector.inc) uses the test
inputs in [the Python check](../tests/advertisement_vector_test.py) and a fixed
nonce. With PyNaCl installed, run `python3 -m unittest tests/advertisement_vector_test.py`;
the C++ decoder checks the same record. Never use the synthetic keys or fixed
nonce in an application: production encryption generates fresh random nonces.

## Signaling

A signaling record starts with `PDSG` and byte `01`, followed by a libsodium
sealed box for the recipient's X25519 key. Its plaintext is the body below
followed by a 64-byte Ed25519 signature; the encryption is randomized.

| Body offset | Bytes | Field |
| --- | --- | --- |
| 0 | 5 | Repeated `PDSG` header and version |
| 5 | 1 | Kind: offer 1, answer 2, candidate 3, cancel 4, reject 5 |
| 6 | 32 each | Sender and recipient Ed25519 public keys |
| 70 | 16 each | Sender instance, recipient instance, attempt identifier |
| 118 | 8 each | Sequence, issued time, expiry time |
| 142 | 4 | Payload length |
| 146 | variable | Payload |

The signature input is `p2pdc/signaling/signature/v1`, followed by BLAKE2b-256
keyed by the capability over `p2pdc/signaling/group-binding/v1`, then the body.
The receiver checks the recipient, signature, lifetime, sender and attempt
binding, and a bounded replay window. Candidate payloads use `PDIC`, byte `01`,
two big-endian 16-bit lengths, then candidate and media-ID bytes; empty
end-of-candidates markers are not encoded by this format.

The first alpha does not chunk or batch signaling records. An encrypted record
is limited to 32,768 bytes, including 263 bytes of envelope overhead; an offer
or answer can therefore contain at most 32,505 SDP bytes. Each candidate is a
separate record, with at most 4,096 candidate bytes and 256 media-ID bytes.
Each peer may send at most 63 ordinary signaling records per attempt, including
its SDP description, so there is room for at most 62 candidate records. Sequence
63 is reserved for an authenticated cancellation. Other queue
and memory limits can cause an attempt to fail sooner. Oversized descriptions,
oversized candidates, or excess records fail the attempt; they are not
silently truncated. A large configuration may therefore fail to connect.

The [signaling plaintext vector](../tests/signaling_vector.inc) contains the
canonical body and signature from the fixed test inputs in
[the C++ signaling test](../tests/signaling_test.cpp). It does not include a
sealed box: sealing uses randomness, so the outer wire bytes vary. With PyNaCl
installed, run `python3 -m unittest tests/signaling_vector_test.py` to check
the body and signature independently.

These records are distinct from the [reliable message frames](FRAMING_V1.md)
sent after a connection is established.
