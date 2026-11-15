import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "android_meson_cross", Path(__file__).resolve().parents[1] / "tools/android_meson_cross.py"
)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AndroidMesonCrossTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.ndk = self.root / "NDK's files"
        self.android = self.root / "target"
        self.glib = self.root / "glib"
        binary = self.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
        binary.mkdir(parents=True)
        for name in ("aarch64-linux-android24-clang", "aarch64-linux-android24-clang++", "llvm-ar", "llvm-strip"):
            (binary / name).touch()
        (self.android / "include").mkdir(parents=True)
        (self.android / "lib/pkgconfig").mkdir(parents=True)
        self.pkg = self.root / "pkg-config"
        self.pkg.touch()

    def test_renders_target_only_paths_and_escapes_quotes(self) -> None:
        output = module.render(self.ndk, self.android, self.glib, 24, self.pkg)
        self.assertIn("NDK\\'s files", output)
        self.assertIn(f"{self.glib}/lib/pkgconfig", output)
        self.assertIn(f"{self.android}/lib/pkgconfig", output)
        self.assertNotIn("/usr/lib/pkgconfig", output)
        self.assertIn("needs_exe_wrapper = true", output)

    def test_rejects_invalid_api_missing_tool_and_unsafe_path(self) -> None:
        with self.assertRaisesRegex(ValueError, "at least 24"):
            module.render(self.ndk, self.android, self.glib, 23, self.pkg)
        with self.assertRaisesRegex(ValueError, "missing tool"):
            module.render(self.ndk, self.android, self.glib, 25, self.pkg)
        with self.assertRaisesRegex(ValueError, "missing target dependency directory"):
            module.render(self.ndk, self.root / "missing", self.glib, 24, self.pkg)
        with self.assertRaisesRegex(ValueError, "newlines"):
            module.quote("bad\npath")

    def test_cli_rejects_invalid_api(self) -> None:
        result = subprocess.run(
            [sys.executable, spec.origin, "--ndk", str(self.ndk), "--android-prefix", str(self.android),
             "--glib-prefix", str(self.glib), "--api", "23"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("Android API must be at least 24", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
