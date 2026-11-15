"""Local-only signaling harness for the experimental browser/native smoke test."""
import argparse
import json
import queue
import subprocess
import tempfile
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


def encode(value):
    if not isinstance(value, str) or not value or len(value) > 32768:
        raise ValueError("invalid signaling field")
    return value.encode("utf-8").hex()


class Harness:
    def __init__(self, executable):
        self.events = queue.Queue(maxsize=128)
        self.result = "pending"
        self.write_lock = threading.Lock()
        # Retained until the child and its reader have stopped in close().
        self.stderr = tempfile.TemporaryFile(mode="w+t")  # noqa: SIM115
        self.process = subprocess.Popen([executable], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr,
                                        text=True, bufsize=1)
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()

    def read_output(self):
        for line in self.process.stdout:
            parts = line.strip().split(" ")
            try:
                if len(parts) == 3 and parts[0] == "D":
                    self.events.put_nowait({"kind": "description", "type": parts[1],
                                            "sdp": bytes.fromhex(parts[2]).decode()})
                elif len(parts) == 3 and parts[0] == "C":
                    self.events.put_nowait({"kind": "candidate", "sdpMid": bytes.fromhex(parts[1]).decode(),
                                            "candidate": bytes.fromhex(parts[2]).decode()})
                elif len(parts) == 2 and parts[0] == "W":
                    data = bytes.fromhex(parts[1])
                    if not data or len(data) > 32768:
                        raise ValueError("invalid wire length")
                    self.events.put_nowait({"kind": "wire", "data": data.hex()})
                elif parts == ["PASS"]:
                    if self.result == "pending":
                        self.result = "exchange"
                else:
                    self.result = "invalid native output"
            except (ValueError, UnicodeError):
                self.result = "invalid native signaling"
            except queue.Full:
                self.result = "native signaling queue full"
        code = self.process.wait()
        if code == 0 and self.result == "exchange":
            self.result = "pass"
        elif code != 0:
            self.stderr.seek(0)
            self.result = "native peer failed: " + self.stderr.read(1024)
        elif self.result == "pending":
            self.result = "native peer exited before exchange"

    def send(self, value):
        if value.get("kind") == "description" and value.get("type") == "answer":
            line = "D answer " + encode(value.get("sdp"))
        elif value.get("kind") == "candidate":
            line = "C " + encode(value.get("sdpMid")) + " " + encode(value.get("candidate"))
        elif value.get("kind") == "done":
            line = "DONE"
        elif value.get("kind") == "stop":
            line = "STOP"
        elif value.get("kind") == "wire":
            data = value.get("data")
            if not isinstance(data, str) or len(data) > 65536:
                raise ValueError("invalid wire")
            decoded = bytes.fromhex(data)
            if not decoded:
                raise ValueError("empty wire")
            line = "W " + decoded.hex()
        else:
            raise ValueError("invalid signaling message")
        with self.write_lock:
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=3)
        if not self.reader.is_alive():
            self.stderr.close()


def handler_for(harness):
    class Handler(SimpleHTTPRequestHandler):
        def json_response(self, data, status=200):
            body = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path != "/interop-events":
                return super().do_GET()
            events = []
            while True:
                try:
                    events.append(harness.events.get_nowait())
                except queue.Empty:
                    break
            self.json_response({"events": events, "result": harness.result})

        def do_POST(self):
            if self.path != "/interop-signal":
                return self.json_response({"error": "not found"}, 404)
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if size <= 0 or size > 131072:
                    raise ValueError("invalid body size")
                value = json.loads(self.rfile.read(size))
                if not isinstance(value, dict):
                    raise TypeError("invalid body")
                harness.send(value)
                self.json_response({"ok": True})
            except (ValueError, TypeError, OSError, json.JSONDecodeError) as error:
                self.json_response({"error": str(error)}, 400)
    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("peer")
    parser.add_argument("--secure", action="store_true")
    args = parser.parse_args()
    harness = Harness(args.peer)
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(harness))
    page = "browser_secure_interop_smoke.html" if args.secure else "browser_interop_smoke.html"
    print(f"http://127.0.0.1:{server.server_port}/tests/{page}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        harness.close()


if __name__ == "__main__":
    main()
