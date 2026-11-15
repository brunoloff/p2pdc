"""Check the documented discovery-key vectors independently of libsodium."""
import hashlib
import unittest

CAPABILITY = bytes(range(32))


class DiscoveryVectorsTest(unittest.TestCase):
    def test_fixed_key(self) -> None:
        digest = hashlib.blake2b(b"p2pdc/discovery/lookup-key/v1", key=CAPABILITY, digest_size=20)
        self.assertEqual(digest.hexdigest(), "ea76a9b4ad1dfbe793c5eae8d96bb51e41b65732")

    def test_rotating_keys_and_separation(self) -> None:
        expected = {
            0: "11c260c6036358b9a09c07042149d9da73d1f953",
            123456789: "bab92718c51c00d6b6d92eb52910ffc0cc9a5618",
        }
        for window, value in expected.items():
            with self.subTest(window=window):
                message = b"p2pdc/discovery/window-key/v2" + window.to_bytes(8, "big")
                digest = hashlib.blake2b(message, key=CAPABILITY, digest_size=20)
                self.assertEqual(digest.hexdigest(), value)
                changed = hashlib.blake2b(message, key=bytes(32), digest_size=20)
                self.assertNotEqual(digest.digest(), changed.digest())


if __name__ == "__main__":
    unittest.main()
