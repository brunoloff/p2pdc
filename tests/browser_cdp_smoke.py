"""Run browser smoke pages with real time using a local Chromium debugging port."""
import argparse
import json
import time
import urllib.request
from typing import Protocol, cast

import websocket


class Connection(Protocol):
    def send(self, value: str) -> object: ...
    def recv(self) -> str | bytes: ...


def call(connection: Connection, number: int, method: str,
         params: dict[str, object]) -> dict[str, object]:
    connection.send(json.dumps({"id": number, "method": method, "params": params}))
    while True:
        message: dict[str, object] = json.loads(connection.recv())
        if message.get("id") == number:
            if "error" in message:
                raise RuntimeError(message["error"])
            return cast(dict[str, object], message["result"])


def run_page(connection: Connection, url: str, timeout: float) -> None:
    call(connection, 1, "Runtime.evaluate", {
        "expression": "if (document.body) document.body.dataset.result = 'pending'",
    })
    call(connection, 2, "Page.navigate", {"url": url})
    deadline = time.monotonic() + timeout
    number = 3
    while time.monotonic() < deadline:
        result = call(connection, number, "Runtime.evaluate", {
            "expression": "document.body ? ({status:document.body.dataset.result,text:document.body.textContent}) : null",
            "returnByValue": True,
        })
        number += 1
        response = result.get("result")
        value = response.get("value") if isinstance(response, dict) else None
        if isinstance(value, dict) and value.get("status") in ("pass", "fail"):
            print(value["text"], flush=True)
            if value["status"] != "pass":
                raise RuntimeError(f"browser smoke failed: {url}")
            return
        time.sleep(0.1)
    raise RuntimeError(f"browser smoke timed out: {url}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("url", nargs="+")
    parser.add_argument("--port", type=int, default=19222)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    with urllib.request.urlopen(f"http://127.0.0.1:{args.port}/json", timeout=5) as response:
        pages = json.load(response)
    connection = websocket.create_connection(pages[0]["webSocketDebuggerUrl"], timeout=5)
    try:
        for url in args.url:
            run_page(connection, url, args.timeout)
    finally:
        connection.close()


if __name__ == "__main__":
    main()
