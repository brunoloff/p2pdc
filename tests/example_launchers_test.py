import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent


class ExampleLauncherTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        shutil.copytree(ROOT / "bin", self.root / "bin")

    def tearDown(self):
        self.temp.cleanup()

    def make_program(self, path, name):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"#!/bin/sh\nprintf '%s:%s\\n' '{name}' \"$1\"\n")
        target.chmod(0o755)
        return target

    def test_native_launcher_prefers_tls_and_forwards_arguments(self):
        self.make_program("build-native/examples/shared-text/p2pdc_shared_text", "normal")
        tls = self.make_program("build-native-tls/examples/shared-text/p2pdc_shared_text", "tls")
        launcher = self.root / "bin/p2pdc_shared_text"
        self.assertEqual(subprocess.check_output([launcher, "argument"], text=True).strip(),
                         "tls:argument")
        tls.unlink()
        self.assertEqual(subprocess.check_output([launcher, "argument"], text=True).strip(),
                         "normal:argument")

    def test_native_launcher_reports_missing_build(self):
        result = subprocess.run([self.root / "bin/p2pdc_echo"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cmake --build", result.stderr)

    def test_browser_launcher_reports_missing_build(self):
        result = subprocess.run([self.root / "bin/p2pdc_shared_text_browser"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("WebAssembly example not found", result.stderr)

    def test_browser_launcher_serves_on_loopback(self):
        app = self.root / "build-qt-wasm/examples/shared-text/p2pdc_shared_text.html"
        app.parent.mkdir(parents=True)
        app.write_text("example page")
        env = dict(os.environ, BROWSER="/bin/true %s")
        process = subprocess.Popen([self.root / "bin/p2pdc_shared_text_browser"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            self.assertIsNotNone(process.stdout)
            line = process.stdout.readline().strip()
            self.assertTrue(line.startswith("Shared Text: http://127.0.0.1:"), line)
            with urlopen(line.removeprefix("Shared Text: "), timeout=5) as response:
                self.assertEqual(response.read(), b"example page")
        finally:
            process.terminate()
            process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main()
