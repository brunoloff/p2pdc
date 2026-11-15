"""Exercise the standalone form against an isolated native peer."""
import json
import re
import sys
import time
import urllib.request

import websocket

from browser_cdp_smoke import call


def evaluate(connection, sequence, expression):
    result = call(connection, sequence, "Runtime.evaluate", {
        "expression": expression,
        "returnByValue": True,
    })
    return result.get("result", {}).get("value")


def wait_for(connection, expression, expected, timeout):
    deadline = time.monotonic() + timeout
    sequence = 10
    while time.monotonic() < deadline:
        value = evaluate(connection, sequence, expression)
        sequence += 1
        if value == expected:
            return
        if value == "fail":
            detail = evaluate(connection, sequence, "document.body.textContent")
            raise RuntimeError(f"browser echo failed: {detail}")
        time.sleep(0.1)
    raise RuntimeError(f"browser echo timed out waiting for {expected}")


def main():
    url, peer_key, role, mode, result_path = sys.argv[1:]
    with urllib.request.urlopen("http://127.0.0.1:19222/json", timeout=5) as response:
        pages = json.load(response)
    connection = websocket.create_connection(pages[0]["webSocketDebuggerUrl"], timeout=5)
    try:
        target = url + "?echo-run=1"
        call(connection, 1, "Page.navigate", {"url": target})
        ready = "location.href === " + json.dumps(target) + " && Boolean(document.getElementById('settings'))"
        wait_for(connection, ready, True, 10)
        initiator = role == "initiator"
        values = {
            "ROLE": role, "CAPABILITY": "05" * 32,
            "SEED": ("01" if initiator else "02") * 32,
            "SECRET": ("03" if initiator else "04") * 32,
            "PEER_KEY": peer_key, "PROXY": "http://127.0.0.1:49412", "NETWORK": "873199",
        }
        if mode != "direct":
            values.update({
                "TURN_HOST": "10.42.0.1", "TURN_PORT": "34789", "TURN_USER": "p2pdc",
                "TURN_PASSWORD": "temporary-test-password", "TURN_TRANSPORT": "tcp",
                "RELAY_POLICY": "required" if mode == "turn" else "automatic",
            })
        expression = "(() => {const f=document.getElementById('settings'); const v=" + json.dumps(values) + "; Object.entries(v).forEach(([k,x])=>f.elements[k].value=x); f.requestSubmit(); return true;})()"
        if not evaluate(connection, 2, expression):
            raise RuntimeError("could not submit echo form")
        wait_for(connection, "document.body.dataset.result || 'pending'", "pass", 45)
        output = evaluate(connection, 3, "document.getElementById('output').textContent")
        exchange = "echo received" if initiator else "echo acknowledged; waiting for peer"
        if not isinstance(output, str) or exchange not in output:
            raise RuntimeError(f"unexpected browser echo output: {output}")
        ambiguous = re.search(r"route: Unknown \[local=(?:PeerReflexive, remote=[A-Za-z]+|[A-Za-z]+, remote=PeerReflexive)\]", output)
        if mode == "direct" and "route: Direct" not in output and not ambiguous:
            raise RuntimeError(f"browser did not report direct or an ambiguous pair: {output}")
        if mode != "direct" and "route: TurnRelayed (TURN used)" not in output and not ambiguous:
            raise RuntimeError(f"browser did not report a relay or ambiguous pair: {output}")
        if re.search(r"route: Direct \[local=(?:PeerReflexive, remote=[A-Za-z]+|[A-Za-z]+, remote=PeerReflexive)\]", output):
            raise RuntimeError(f"peer-reflexive route was reported as direct: {output}")
        with open(result_path, "w", encoding="utf-8") as result:
            result.write(output)
        print(f"Standalone browser {role} exchanged echo with truthful route status", flush=True)
    finally:
        connection.close()


if __name__ == "__main__":
    main()
