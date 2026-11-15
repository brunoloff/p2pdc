# Reliable message framing, version 1

Reliable channel messages, including authentication records, are carried as
one or more ordered binary frames. Each frame has a 19-byte header followed by
its fragment bytes. Integers use network byte order (most significant byte first).

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 2 | Magic bytes `50 44` (`PD`) |
| 2 | 1 | Version `01` |
| 3 | 8 | Message identifier, unsigned |
| 11 | 4 | Total message size, unsigned |
| 15 | 4 | Fragment offset, unsigned |
| 19 | remainder | Fragment payload |

The total size must fit the channel's configured reassembly limit, and the frame
must fit the local frame limit. An offset cannot exceed the total size, and a fragment cannot
extend beyond it. A nonempty message cannot have an empty fragment. An empty
message is represented by a single frame with total size and offset zero and no
payload. The reassembler requires the first offset to be zero, then contiguous
offsets with the same identifier and total size; an invalid sequence resets it.

The reassembled channel message begins with a one-byte gate marker: `00` for
handshake control, or `01` for application bytes. Application data is accepted
only after the handshake succeeds, and its size is checked against the
negotiated application limit. The frame's total size includes this marker.
The [handshake reference](HANDSHAKE_V1.md) describes control records.

The [version-1 vectors](../tests/frame_vectors.inc) give hexadecimal payloads and
wire bytes for an empty message and a two-fragment binary message. The C++ codec
tests and an independent Python `struct` test both check those same vectors.
These vectors describe framing only; they do not specify discovery, signaling,
authentication, or compatibility with another application's protocol.

Run the independent check from the library root with
`python3 -m unittest tests/frame_vectors_test.py`.
