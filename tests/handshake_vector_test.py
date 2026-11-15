"""Verify the published handshake record with PyNaCl and Python struct."""
import struct
import unittest
from pathlib import Path

from nacl.exceptions import BadSignatureError
from nacl.signing import SigningKey

VECTOR = Path(__file__).with_name("handshake_vector.inc")


class HandshakeVectorTest(unittest.TestCase):
    def test_hello_signature_and_context_binding(self) -> None:
        lines = VECTOR.read_text(encoding="utf-8").splitlines()
        self.assertTrue(all(line.startswith('"') and line.endswith('"') for line in lines))
        expected = bytes.fromhex("".join(line[1:-1] for line in lines))
        offerer = SigningKey(bytes([1]) * 32)
        answerer = SigningKey(bytes([2]) * 32)
        context = (bytes(offerer.verify_key) + bytes(answerer.verify_key)
                   + b"\x01" + bytes(15) + b"\x02" + bytes(15) + b"\x03" + bytes(15)
                   + b"\x04" + bytes(31) + b"\x05" + bytes(31) + b"\x06" + bytes(31))
        body = b"PDHS\x01\x01\x01" + struct.pack(">I", 100) + b"\x07" + bytes(31) + bytes(32)
        signed = b"p2pdc/handshake/signature/v1" + context + body
        signature = offerer.sign(signed).signature
        self.assertEqual(body + signature, expected)
        offerer.verify_key.verify(signed, signature)
        with self.assertRaises(BadSignatureError):
            offerer.verify_key.verify(signed + b"\x00", signature)


if __name__ == "__main__":
    unittest.main()
