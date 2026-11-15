"""Verify the fixed advertisement test vector with PyNaCl."""
import hashlib
import struct
import unittest
from pathlib import Path

from nacl.bindings import (
    crypto_aead_xchacha20poly1305_ietf_decrypt,
    crypto_aead_xchacha20poly1305_ietf_encrypt,
)
from nacl.exceptions import CryptoError
from nacl.signing import SigningKey

VECTOR = Path(__file__).with_name("advertisement_vector.inc")


class AdvertisementVectorTest(unittest.TestCase):
    def test_fixed_nonce_record_and_tamper_rejection(self) -> None:
        lines = VECTOR.read_text(encoding="utf-8").splitlines()
        self.assertTrue(all(line.startswith('"') and line.endswith('"') for line in lines))
        expected = bytes.fromhex("".join(line[1:-1] for line in lines))
        capability, nonce, header = b"\x07" + bytes(31), bytes(range(24)), b"PDAD\x01"
        sender = SigningKey(bytes([1]) * 32)
        body = (b"\x01" + bytes(sender.verify_key) + b"\x02" + bytes(31)
                + b"\x03" + bytes(15) + b"\x04" + bytes(19)
                + struct.pack(">QQQQ", 0x0102030405060708, 9, 100, 150))
        group = hashlib.blake2b(b"p2pdc/advertisement/group-binding/v1", key=capability, digest_size=32).digest()
        signature = sender.sign(b"p2pdc/advertisement/signature/v1" + group + body).signature
        key = hashlib.blake2b(b"p2pdc/capability-encryption/key/v1", key=capability, digest_size=32).digest()
        aad = b"p2pdc/advertisement/aead/v1" + header
        ciphertext = crypto_aead_xchacha20poly1305_ietf_encrypt(body + signature, aad, nonce, key)
        self.assertEqual(header + nonce + ciphertext, expected)
        self.assertEqual(crypto_aead_xchacha20poly1305_ietf_decrypt(ciphertext, aad, nonce, key), body + signature)
        with self.assertRaises(CryptoError):
            crypto_aead_xchacha20poly1305_ietf_decrypt(ciphertext[:-1] + bytes([ciphertext[-1] ^ 1]), aad, nonce, key)


if __name__ == "__main__":
    unittest.main()
