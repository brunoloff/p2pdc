"""Check browser input validation and C++ signing-key derivation."""
import json
import sys
import time
import urllib.request

import websocket

from browser_cdp_smoke import call


def value(connection, number, expression):
    result = call(connection, number, "Runtime.evaluate", {
        "expression": expression,
        "returnByValue": True,
    })
    return result.get("result", {}).get("value")


def wait_for(connection, expression, expected, timeout):
    deadline = time.monotonic() + timeout
    number = 10
    while time.monotonic() < deadline:
        if value(connection, number, expression) == expected:
            return
        number += 1
        time.sleep(0.1)
    raise RuntimeError(f"browser key derivation timed out: {expected}")


def main():
    url, expected_key = sys.argv[1:]
    with urllib.request.urlopen("http://127.0.0.1:19222/json", timeout=5) as response:
        pages = json.load(response)
    connection = websocket.create_connection(pages[0]["webSocketDebuggerUrl"], timeout=5)
    try:
        target = url + "?key-check=1"
        call(connection, 1, "Page.navigate", {"url": target})
        ready = "location.href === " + json.dumps(target) + " && Boolean(document.getElementById('derive'))"
        wait_for(connection, ready, True, 10)
        value(connection, 2, "document.getElementById('derive').click()")
        wait_for(connection, "document.getElementById('output').textContent.includes('32-byte')", True, 5)
        expression = "document.getElementById('settings').elements.SEED.value='" + "01" * 32 + "'; document.getElementById('derive').click()"
        value(connection, 3, expression)
        wait_for(connection, "document.getElementById('output').textContent.includes(" + json.dumps(expected_key) + ")", True, 15)
        print("Browser validates seeds and derives the same public key", flush=True)
    finally:
        connection.close()


if __name__ == "__main__":
    main()
