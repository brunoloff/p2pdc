// Round-trip times from a real browser's getStats(), between two transports in one headless
// Chromium page. Opt-in, as it starts a browser:
//   P2PDC_TEST_CHROMIUM=/usr/bin/chromium node --test tests/webrtc_round_trip_browser_test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { chromiumPath, openChromiumPage } from "./support/chromium_page.mjs";

// Runs in the page: connects an offerer and an answerer, then watches their routes for a while.
async function roundTripsInPage(watchMs) {
    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    for (let i = 0; i < 100 && !window.p2pdcRtc; ++i) await sleep(50);
    const { BrowserWebRtcTransport } = window.p2pdcRtc;
    const routes = { offerer: [], answerer: [] }, errors = [], transports = {};
    const handlers = (role, other) => ({
        onDescription: (value) => transports[other].setRemoteDescription(value).catch((e) => errors.push(String(e))),
        onCandidate: (value) => transports[other].addRemoteCandidate(value).catch((e) => errors.push(String(e))),
        onState() {}, onBinary() {}, onWritable() {}, onError: (reason) => errors.push(reason),
        onRoute: (route) => routes[role].push({ at: performance.now(), ms: route.roundTripMs, current: route.current }),
    });
    for (const [role, other] of [["offerer", "answerer"], ["answerer", "offerer"]])
        transports[role] = new BrowserWebRtcTransport({ role, relayPolicy: "automatic", iceServers: [] }, handlers(role, other));
    await transports.offerer.startOffer();
    const measured = (list) => list.filter((route) => typeof route.ms === "number");
    for (let i = 0; i < 200 && !(measured(routes.offerer).length && measured(routes.answerer).length); ++i) await sleep(50);
    const start = performance.now();
    await sleep(watchMs);
    const later = (list) => measured(list).filter((route) => route.at > start).length;
    const result = { errors, offerer: measured(routes.offerer).at(-1)?.ms ?? null, answerer: measured(routes.answerer).at(-1)?.ms ?? null,
                     refreshes: Math.min(later(routes.offerer), later(routes.answerer)) };
    for (const transport of Object.values(transports)) transport.close();
    return result;
}

test("Chromium reports the selected pair's round-trip time, refreshed every couple of seconds",
     { skip: !chromiumPath && "set P2PDC_TEST_CHROMIUM", timeout: 60000 }, async (t) => {
         const page = await openChromiumPage("tests/webrtc_round_trip_page.html");
         t.after(() => page.close());
         const result = await page.evaluate(roundTripsInPage, 6500);
         assert.deepEqual(result.errors, []);
         for (const role of ["offerer", "answerer"]) {
             assert.equal(typeof result[role], "number", `${role} has a round-trip time`);
             assert.ok(result[role] >= 0 && result[role] < 1000, `${role}: ${result[role]} ms on loopback`);
         }
         assert.ok(result.refreshes >= 2 && result.refreshes <= 5, `refreshed ${result.refreshes} times in 6.5 s`);
     });
