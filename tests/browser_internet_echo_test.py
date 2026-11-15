import importlib.util
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

spec = importlib.util.spec_from_file_location("browser_internet_echo", Path(__file__).with_name("browser_internet_echo.py"))
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BrowserInternetEchoTest(unittest.TestCase):
    def test_settings_require_a_proxy_identity_and_valid_role(self) -> None:
        env = {"P2PDC_ECHO_" + name: name.lower() for name in module.REQUIRED}
        env["P2PDC_ECHO_ROLE"] = "initiator"
        env["P2PDC_TEST_BROWSER_PROXY"] = "https://proxy.example"
        values = module.settings(env)
        self.assertEqual(values["PROXY"], "https://proxy.example")
        self.assertEqual(values["ROLE"], "initiator")
        del env["P2PDC_ECHO_SEED"]
        with self.assertRaisesRegex(ValueError, "missing browser echo configuration"):
            module.settings(env)
        env["P2PDC_ECHO_SEED"] = "seed"
        env["P2PDC_ECHO_ROLE"] = "invalid"
        with self.assertRaisesRegex(ValueError, "invalid browser echo role"):
            module.settings(env)

    def test_server_exposes_only_the_example_files(self) -> None:
        server = ThreadingHTTPServer(("127.0.0.1", 0), module.ExampleHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(thread.join)
        self.addCleanup(server.shutdown)
        base = f"http://127.0.0.1:{server.server_port}"
        with urllib.request.urlopen(base + "/examples/echo/index.html", timeout=5) as response:
            self.assertEqual(response.status, 200)
        for method in ("GET", "HEAD"):
            request = urllib.request.Request(base + "/README.md", method=method)
            with self.assertRaises(urllib.error.HTTPError) as failure:
                urllib.request.urlopen(request, timeout=5)
            self.assertEqual(failure.exception.code, 404)
            failure.exception.close()

    def test_rejects_unknown_browser(self) -> None:
        with self.assertRaisesRegex(ValueError, "browser must be chromium or firefox"):
            module.run_page("http://127.0.0.1/", {}, "unknown")


if __name__ == "__main__":
    unittest.main()
