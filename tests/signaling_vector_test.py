"""Verify a signaling plaintext vector independently with PyNaCl."""
import hashlib
import struct
import unittest
from pathlib import Path

from nacl.exceptions import BadSignatureError
from nacl.signing import SigningKey

VECTOR = Path(__file__).with_name("signaling_vector.inc")


class SignalingVectorTest(unittest.TestCase):
    def test_canonical_body_and_signature(self) -> None:
        lines = VECTOR.read_text(encoding="utf-8").splitlines()
        self.assertTrue(all(line.startswith('"') and line.endswith('"') for line in lines))
        expected = bytes.fromhex("".join(line[1:-1] for line in lines))
        sender = SigningKey(b"\x07" + bytes(31))
        recipient = SigningKey(b"\x08" + bytes(31))
        body = (b"PDSG\x01\x01" + bytes(sender.verify_key) + bytes(recipient.verify_key)
                + b"\x01" + bytes(15) + b"\x02" + bytes(15) + b"\x03" + bytes(15)
                + struct.pack(">QQQI", 0x0102030405060708, 100, 150, 3) + b"\x00\xff\x07")
        context = hashlib.blake2b(b"p2pdc/signaling/group-binding/v1",
                                  key=b"\x09" + bytes(31), digest_size=32).digest()
        message = b"p2pdc/signaling/signature/v1" + context + body
        signature = sender.sign(message).signature
        self.assertEqual(body + signature, expected)
        sender.verify_key.verify(message, signature)
        with self.assertRaises(BadSignatureError):
            sender.verify_key.verify(message + b"\x00", signature)


if __name__ == "__main__":
    unittest.main()
