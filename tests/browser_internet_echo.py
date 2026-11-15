#!/usr/bin/env python3
"""Run the shared C++ echo example in a temporary browser."""
import os
import shutil
import socket
import socketserver
import sys
import threading
from collections.abc import Mapping
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {"/examples/echo/index.html", "/build-wasm-auth/p2pdc_echo.js",
           "/build-wasm-auth/p2pdc_echo.wasm"}
REQUIRED = ("ROLE", "CAPABILITY", "SEED", "SECRET", "PEER_KEY")
OPTIONAL = ("NETWORK", "STUN_HOST", "STUN_PORT", "TURN_HOST", "TURN_PORT", "TURN_USER",
            "TURN_PASSWORD", "TURN_TRANSPORT", "RELAY_POLICY")


def settings(env: Mapping[str, str]) -> dict[str, str]:
    result = {name: env.get("P2PDC_ECHO_" + name, "") for name in REQUIRED + OPTIONAL}
    result["PROXY"] = env.get("P2PDC_TEST_BROWSER_PROXY", "")
    if any(not result[name] for name in (*REQUIRED, "PROXY")):
        raise ValueError("missing browser echo configuration")
    if result["ROLE"] not in ("initiator", "responder"):
        raise ValueError("invalid browser echo role")
    return {name: value for name, value in result.items() if value}


class ExampleHandler(SimpleHTTPRequestHandler):
    def __init__(self, request: socket.socket | tuple[bytes, socket.socket],
                 client_address: tuple[str, int], server: socketserver.BaseServer) -> None:
        super().__init__(request, client_address, server, directory=str(ROOT))

    def do_GET(self) -> None:
        if urlsplit(self.path).path not in ALLOWED:
            self.send_error(404)
            return
        super().do_GET()

    def do_HEAD(self) -> None:
        if urlsplit(self.path).path not in ALLOWED:
            self.send_error(404)
            return
        super().do_HEAD()


def run_page(url: str, values: Mapping[str, str], browser_name: str) -> str:
    if browser_name not in ("chromium", "firefox"):
        raise ValueError("browser must be chromium or firefox")
    with sync_playwright() as playwright:
        engine = playwright.chromium if browser_name == "chromium" else playwright.firefox
        executable = shutil.which("chromium") if browser_name == "chromium" else None
        if browser_name == "chromium" and not executable:
            raise RuntimeError("chromium is not installed")
        args = ["--renderer-process-limit=1"] if browser_name == "chromium" else []
        browser = engine.launch(headless=True, executable_path=executable, args=args)
        try:
            page = browser.new_page()
            if os.environ.get("P2PDC_TEST_ICE_TRACE") == "1":
                page.on("console", lambda message: print(message.text, file=sys.stderr, flush=True)
                        if message.text.startswith("p2pdc-ice:") else None)
                page.add_init_script(path=str(ROOT / "tests/browser_ice_trace.js"))
            page.goto(url, wait_until="load", timeout=10000)
            for name, value in values.items():
                field = page.locator(f'[name="{name}"]')
                if name in ("ROLE", "TURN_TRANSPORT", "RELAY_POLICY"):
                    field.select_option(value)
                else:
                    field.fill(value)
            page.get_by_role("button", name="Start", exact=True).click()
            page.wait_for_function("['pass', 'fail'].includes(document.body.dataset.result)", timeout=70000)
            output = page.locator("#output").text_content() or ""
            if page.locator("body").get_attribute("data-result") != "pass":
                raise RuntimeError(f"browser echo failed: {output}")
            return output
        finally:
            browser.close()


def main() -> int:
    try:
        values = settings(os.environ)
        server = ThreadingHTTPServer(("127.0.0.1", 0), ExampleHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/examples/echo/index.html"
            output = run_page(url, values, os.environ.get("P2PDC_TEST_BROWSER_ENGINE", "chromium"))
        finally:
            server.shutdown()
            thread.join()
            server.server_close()
        print(output, end="", flush=True)
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"browser internet echo: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
