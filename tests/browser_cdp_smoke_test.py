import json
import unittest

from browser_cdp_smoke import call, run_page


class FakeConnection:
    def __init__(self, responses):
        self.responses = iter(responses)
        self.sent = []

    def send(self, value):
        self.sent.append(json.loads(value))

    def recv(self):
        return json.dumps(next(self.responses))


class BrowserCdpSmokeTest(unittest.TestCase):
    def test_call_ignores_events_and_returns_response(self):
        connection = FakeConnection([{"method": "Page.event"}, {"id": 7, "result": {"ok": True}}])
        self.assertEqual(call(connection, 7, "Page.test", {}), {"ok": True})
        self.assertEqual(connection.sent[0]["method"], "Page.test")

    def test_call_reports_protocol_error(self):
        connection = FakeConnection([{"id": 7, "error": {"message": "failed"}}])
        with self.assertRaises(RuntimeError):
            call(connection, 7, "Page.test", {})

    def test_page_accepts_pass_and_rejects_failure(self):
        def connection(status):
            return FakeConnection([{"id": 1, "result": {}}, {"id": 2, "result": {}},
                                   {"id": 3, "result": {"result": {"value": {
                                       "status": status, "text": status}}}}])
        run_page(connection("pass"), "http://127.0.0.1/test", 1)
        with self.assertRaises(RuntimeError):
            run_page(connection("fail"), "http://127.0.0.1/test", 1)


if __name__ == "__main__":
    unittest.main()
