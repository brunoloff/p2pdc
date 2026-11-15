# Authenticated handshake, version 1

The current handshake is provisional. Both peers authenticate after their
signed offer and answer have been installed into WebRTC and before application
messages are delivered. The signed context binds the authorized Ed25519 keys,
instance IDs, attempt ID, group, and hashes of the exact offer and answer SDP
bytes. The SDP hashes include the WebRTC fingerprints; the WebRTC backend must
enforce the fingerprints in the installed descriptions.

Each control message is a reliable framed channel message whose reassembled
payload begins with `00`, followed by this 139-byte signed record:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 5 | `PDHS` and version `01` |
| 5 | 1 | Kind: hello `01`, confirmation `02` |
| 6 | 1 | Role: offerer `01`, answerer `02` |
| 7 | 4 | Maximum application-message size, unsigned big-endian |
| 11 | 32 | Sender nonce |
| 43 | 32 | Peer nonce: all zero for hello, nonzero for confirmation |
| 75 | 64 | Ed25519 signature |

The signature input is the ASCII string `p2pdc/handshake/signature/v1` with no
terminating NUL, followed in order by the offerer and answerer signing keys
(32 bytes each), their instance IDs (16 bytes each), attempt ID (16 bytes),
group hash (32 bytes), offer hash (32 bytes), answer hash (32 bytes), and the
75-byte record body. The group hash is BLAKE2b-256 keyed by the capability over
`p2pdc/handshake/group/v1`. Each SDP hash is unkeyed BLAKE2b-256 over
`p2pdc/handshake/sdp/v1` followed by the exact SDP bytes.

Each peer sends a hello with a fresh nonzero nonce, then confirms both its own
nonce and the peer's nonce after receiving the peer's hello. Confirmation also
repeats the sender's receive limit. A connection becomes ready after its own
confirmation is sent and the peer's matching confirmation is received. The
application-message limit is the smaller of the two receive limits. Invalid
records or state transitions terminate the attempt.

The [test-only hello vector](../tests/handshake_vector.inc) is a record without
the channel marker or outer framing, using the fixed inputs in
[the C++ handshake test](../tests/handshake_test.cpp). With PyNaCl installed,
`python3 -m unittest tests/handshake_vector_test.py` independently checks its
body and signature. The vector's synthetic identities and nonces are not for
real connections. The outer framing and marker are described in
[version-1 framing](FRAMING_V1.md).
