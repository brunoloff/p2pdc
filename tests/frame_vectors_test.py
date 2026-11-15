"""Independently verify the published C++ frame vectors with Python's struct."""
import re
import struct
import unittest
from pathlib import Path

PATTERN = re.compile(r'^FRAME_VECTOR\((\w+), (0x[0-9a-f]+), (\d+), (\d+), "([0-9a-f]*)", "([0-9a-f]+)"\)$')
VECTORS = Path(__file__).with_name("frame_vectors.inc")


class FrameVectorsTest(unittest.TestCase):
    def test_vectors_encode_and_decode(self) -> None:
        lines = VECTORS.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(lines), 3)
        for line in lines:
            match = PATTERN.fullmatch(line)
            self.assertIsNotNone(match)
            assert match is not None
            name, ident, total, offset, payload_hex, wire_hex = match.groups()
            with self.subTest(name=name):
                payload, wire = bytes.fromhex(payload_hex), bytes.fromhex(wire_hex)
                self.assertEqual(wire, b"PD\x01" + struct.pack(">QII", int(ident, 16), int(total), int(offset)) + payload)
                self.assertEqual(struct.unpack(">QII", wire[3:19]), (int(ident, 16), int(total), int(offset)))
                self.assertEqual(wire[19:], payload)

    def test_truncated_header_is_not_unpackable(self) -> None:
        with self.assertRaises(struct.error):
            struct.unpack(">QII", bytes.fromhex("0000000000000000"))


if __name__ == "__main__":
    unittest.main()
