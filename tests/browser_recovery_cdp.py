"""Interrupt an isolated proxy and verify the browser's recovery lifecycle."""
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

import websocket

from browser_cdp_smoke import call


def wait_stage(connection, stage, timeout):
    deadline = time.monotonic() + timeout
    sequence = 10
    while time.monotonic() < deadline:
        result = call(connection, sequence, "Runtime.evaluate", {
            "expression": "document.body ? ({stage:document.body.dataset.stage,status:document.body.dataset.result,text:document.body.textContent}) : null",
            "returnByValue": True,
        })
        sequence += 1
        value = result.get("result", {}).get("value")
        if isinstance(value, dict):
            if value.get("status") == "fail":
                raise RuntimeError(f"browser failed: {value.get('text')}")
            if value.get("stage") == stage or (stage == "pass" and value.get("status") == "pass"):
                print(f"browser reached {stage}", flush=True)
                return
        time.sleep(0.1)
    raise RuntimeError(f"browser timed out waiting for {stage}")


def wait_proxy_down():
    for _ in range(100):
        try:
            with urllib.request.urlopen("http://127.0.0.1:49412/", timeout=0.2):
                pass
        except (urllib.error.URLError, OSError):
            return
        time.sleep(0.05)
    raise RuntimeError("isolated proxy did not stop")


def main():
    proxy_pid, log_path, fifo_path, url = sys.argv[1:]
    with urllib.request.urlopen("http://127.0.0.1:19222/json", timeout=5) as response:
        pages = json.load(response)
    connection = websocket.create_connection(pages[0]["webSocketDebuggerUrl"], timeout=5)
    replacement = None
    try:
        call(connection, 1, "Page.navigate", {"url": url})
        wait_stage(connection, "await-outage", 40)
        os.killpg(int(proxy_pid), signal.SIGTERM)
        wait_proxy_down()
        wait_stage(connection, "reconnecting", 10)
        with open(log_path, "w", encoding="utf-8") as log:
            replacement = subprocess.Popen(
                ["dhtnode", "-n", "873198", "-p", "49411", "-b", "127.0.0.1:49413", "--proxyserver", "49412"],
                stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            )
            wait_stage(connection, "recovered", 35)
            with open(fifo_path, "w", encoding="utf-8") as fifo:
                fifo.write("STOP\n")
            wait_stage(connection, "pass", 10)
    finally:
        if replacement is not None:
            if replacement.poll() is None:
                os.killpg(replacement.pid, signal.SIGTERM)
            replacement.wait(timeout=5)
        connection.close()


if __name__ == "__main__":
    main()
