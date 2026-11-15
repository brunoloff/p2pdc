"""Run a smoke page in the installed WebKitGTK engine under a virtual display."""
import argparse
import json
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.1")
from gi.repository import GLib, Gtk, WebKit2


class SmokePage:
    def __init__(self, url: str, timeout: int) -> None:
        self.result = 1
        self.pending = False
        self.last_error = ""
        self.window = Gtk.Window()
        self.view = WebKit2.WebView()
        self.view.get_settings().set_enable_webrtc(True)
        self.window.add(self.view)
        self.window.show_all()
        self.view.connect("load-failed", self.load_failed)
        self.view.load_uri(url)
        GLib.timeout_add(200, self.poll)
        GLib.timeout_add_seconds(timeout, self.timed_out)

    def load_failed(self, _view: WebKit2.WebView, _event: object, uri: str,
                    error: GLib.Error) -> bool:
        print(f"WebKit failed to load {uri}: {error}", file=sys.stderr)
        Gtk.main_quit()
        return False

    def timed_out(self) -> bool:
        print(f"WebKit smoke page timed out: {self.last_error}", file=sys.stderr)
        Gtk.main_quit()
        return False

    def poll(self) -> bool:
        if not self.pending:
            self.pending = True
            self.view.evaluate_javascript(
                "JSON.stringify({status: document.body?.dataset.result, "
                "text: document.body?.textContent, debug: document.body?.dataset.debug, "
                "rtc: typeof RTCPeerConnection, secure: isSecureContext})",
                -1, None, None, None, self.evaluated, None)
        return True

    def evaluated(self, view: WebKit2.WebView, result: object, _data: object) -> None:
        self.pending = False
        try:
            value = view.evaluate_javascript_finish(result)
            data = json.loads(value.to_string())
        except (GLib.Error, TypeError, ValueError) as error:
            self.last_error = str(error)  # Navigation can temporarily remove the JS context.
            return
        if data.get("status") in ("pass", "fail"):
            print(data.get("text"), data.get("debug", ""), "RTC:", data.get("rtc"),
                  "secure:", data.get("secure"), flush=True)
            self.result = 0 if data["status"] == "pass" else 1
            Gtk.main_quit()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--timeout", type=int, default=35)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    page = SmokePage(args.url, args.timeout)
    Gtk.main()
    page.window.destroy()
    return page.result


if __name__ == "__main__":
    raise SystemExit(main())
