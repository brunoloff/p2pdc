"""Run a smoke page in a temporary headless browser instance."""
import argparse
import shutil
from typing import cast

from playwright.sync_api import Page, sync_playwright


def run_page(page: Page, url: str, timeout: float, expect_failure: bool,
             expected_ice_error: int) -> None:
    page.on("console", lambda message: print(f"browser {message.type}: {message.text}", flush=True))
    page.on("pageerror", lambda error: print(f"browser error: {error}", flush=True))
    page.add_init_script("""{
        const Original = window.RTCPeerConnection;
        window.p2pdcIceErrors = [];
        window.p2pdcRelayCandidates = 0;
        window.RTCPeerConnection = new Proxy(Original, { construct(target, args) {
            const peer = Reflect.construct(target, args);
            peer.addEventListener('icecandidateerror', event => {
                window.p2pdcIceErrors.push(event.errorCode);
                console.error('ICE candidate error', event.errorCode, event.errorText, event.url);
            });
            peer.addEventListener('icecandidate', event => {
                if (event.candidate?.candidate?.includes(' typ relay ')) ++window.p2pdcRelayCandidates;
            });
            return peer;
        }});
    }""")
    page.goto(url, wait_until="load", timeout=timeout * 1000)
    page.wait_for_function(
        "document.body && ['pass', 'fail'].includes(document.body.dataset.result)",
        timeout=timeout * 1000,
    )
    status = page.locator("body").get_attribute("data-result")
    result = page.locator("body").text_content()
    print(result, flush=True)
    expected = "fail" if expect_failure else "pass"
    if status != expected:
        raise RuntimeError(f"browser smoke expected {expected}: {url}")
    if expect_failure:
        observed = cast(dict[str, int], page.evaluate(
            "code => ({errors: window.p2pdcIceErrors.filter(value => value === code).length, "
            "relays: window.p2pdcRelayCandidates})", expected_ice_error
        ))
        if observed["errors"] < 1 or observed["relays"] != 0:
            raise RuntimeError(f"untrusted TLS control lacked expected ICE evidence: {observed}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--profile")
    parser.add_argument("--engine", choices=("firefox", "chromium"), default="firefox")
    parser.add_argument("--expect-failure", action="store_true")
    parser.add_argument("--expect-ice-error", type=int, default=701)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    with sync_playwright() as playwright:
        browser_type = playwright.firefox if args.engine == "firefox" else playwright.chromium
        options = ({"firefox_user_prefs": {"network.dns.localDomains": "turn.test",
                                           "media.peerconnection.ice.loopback": True}}
                   if args.engine == "firefox" else
                   {"executable_path": shutil.which("chromium"),
                    "args": ["--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
                             "--renderer-process-limit=1", "--host-resolver-rules=MAP turn.test 127.0.0.1",
                             "--disable-features=WebRtcHideLocalIpsWithMdns"]})
        if args.profile:
            context = browser_type.launch_persistent_context(args.profile, headless=True, **options)
            try:
                run_page(context.new_page(), args.url, args.timeout, args.expect_failure,
                         args.expect_ice_error)
            finally:
                context.close()
            return
        browser = browser_type.launch(headless=True, **options)
        try:
            run_page(browser.new_page(), args.url, args.timeout, args.expect_failure,
                     args.expect_ice_error)
        finally:
            browser.close()


if __name__ == "__main__":
    main()
