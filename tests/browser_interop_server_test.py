import io
import threading
import unittest
from unittest.mock import Mock

from browser_interop_server import Harness, encode


class SignalingTests(unittest.TestCase):
    def test_encode_validates_input(self):
        self.assertEqual(encode("å"), "c3a5")
        for value in ("", None, "x" * 32769):
            with self.subTest(value=type(value)), self.assertRaises(ValueError):
                encode(value)

    def test_signaling_messages_and_bad_input(self):
        harness = object.__new__(Harness)
        harness.process = Mock()
        harness.process.stdin = io.StringIO()
        harness.write_lock = threading.Lock()
        harness.send({"kind": "description", "type": "answer", "sdp": "sdp"})
        harness.send({"kind": "candidate", "sdpMid": "0", "candidate": "candidate"})
        harness.send({"kind": "done"})
        harness.send({"kind": "stop"})
        harness.send({"kind": "wire", "data": "00ff"})
        self.assertEqual(harness.process.stdin.getvalue(),
                         "D answer 736470\nC 30 63616e646964617465\nDONE\nSTOP\nW 00ff\n")
        for value in ({"kind": "description", "type": "offer", "sdp": "sdp"},
                      {"kind": "candidate", "sdpMid": None, "candidate": "x"},
                      {"kind": "wire", "data": "xyz"},
                      {"kind": "wire", "data": ""}, {"kind": "unknown"}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                harness.send(value)

    def test_exit_status_must_confirm_exchange(self):
        for lines, code, expected in (("PASS\n", 0, "pass"), ("PASS\n", 1, "native peer failed"),
                                      ("", 0, "native peer exited before exchange")):
            with self.subTest(lines=lines, code=code):
                harness = object.__new__(Harness)
                harness.events = Mock()
                harness.result = "pending"
                harness.stderr = io.StringIO("error")
                harness.process = Mock(stdout=io.StringIO(lines), stderr=io.StringIO("error"))
                harness.process.wait.return_value = code
                harness.read_output()
                self.assertTrue(harness.result.startswith(expected))

    def test_wire_event_and_invalid_record(self):
        harness = object.__new__(Harness)
        harness.events = Mock()
        harness.result = "pending"
        harness.process = Mock(stdout=io.StringIO("W 00ff\nPASS\n"), stderr=io.StringIO())
        harness.process.wait.return_value = 0
        harness.read_output()
        harness.events.put_nowait.assert_called_once_with({"kind": "wire", "data": "00ff"})
        self.assertEqual(harness.result, "pass")
        harness.result = "pending"
        harness.process.stdout = io.StringIO("W xyz\n")
        harness.read_output()
        self.assertEqual(harness.result, "invalid native signaling")
        harness.result = "pending"
        harness.process.stdout = io.StringIO("W xyz\nPASS\n")
        harness.read_output()
        self.assertEqual(harness.result, "invalid native signaling")

    def test_queue_overflow_stays_failed(self):
        import queue
        harness = object.__new__(Harness)
        harness.events = queue.Queue(maxsize=1)
        harness.result = "pending"
        harness.process = Mock(stdout=io.StringIO("W 00\nW 01\nPASS\n"), stderr=io.StringIO())
        harness.process.wait.return_value = 0
        harness.read_output()
        self.assertEqual(harness.result, "native signaling queue full")


if __name__ == "__main__":
    unittest.main()
