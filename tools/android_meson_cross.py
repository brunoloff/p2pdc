"""Print a Meson cross file for Android arm64 dependency builds on Linux."""
import argparse
import shutil
from pathlib import Path

CROSS_FILE = """[binaries]
c = {c}
cpp = {cpp}
ar = {ar}
strip = {strip}
pkg-config = {pkg}

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = [{packages}]
have_c99_vsnprintf = true

[built-in options]
c_args = [{include}]
cpp_args = [{include}]
c_link_args = [{library}]
cpp_link_args = [{library}]
"""


def quote(value: str) -> str:
    if any(char in value for char in "\n\r\0"):
        raise ValueError("paths cannot contain newlines or NUL")
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"


def render(ndk: Path, android: Path, glib: Path, api: int, pkg_config: Path) -> str:
    if api < 24:
        raise ValueError("Android API must be at least 24")
    binary = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    tools = [binary / f"aarch64-linux-android{api}-clang",
             binary / f"aarch64-linux-android{api}-clang++",
             binary / "llvm-ar", binary / "llvm-strip", pkg_config]
    for tool in tools:
        if not tool.is_file():
            raise ValueError(f"missing tool: {tool}")
    for directory in (android / "include", android / "lib/pkgconfig"):
        if not directory.is_dir():
            raise ValueError(f"missing target dependency directory: {directory}")
    c, cpp, ar, strip, pkg = (quote(str(tool)) for tool in tools)
    packages = ", ".join(quote(str(prefix / "lib/pkgconfig")) for prefix in (glib, android))
    include = quote(f"-I{android / 'include'}")
    library = quote(f"-L{android / 'lib'}")
    return CROSS_FILE.format(c=c, cpp=cpp, ar=ar, strip=strip, pkg=pkg, packages=packages,
                             include=include, library=library)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", required=True, type=Path)
    parser.add_argument("--android-prefix", required=True, type=Path)
    parser.add_argument("--glib-prefix", required=True, type=Path)
    parser.add_argument("--api", type=int, default=24)
    args = parser.parse_args()
    pkg_config = shutil.which("pkg-config")
    if not pkg_config:
        parser.error("pkg-config is required")
    try:
        output = render(args.ndk.resolve(), args.android_prefix.resolve(),
                        args.glib_prefix.resolve(), args.api, Path(pkg_config).resolve())
    except ValueError as error:
        parser.error(str(error))
    print(output, end="")


if __name__ == "__main__":
    main()
