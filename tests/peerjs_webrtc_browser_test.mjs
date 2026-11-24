// The PeerJS backend with real WebRTC, in headless Chromium, against the local strict PeerJS
// server. Opt-in, as it starts a browser:
//   P2PDC_TEST_CHROMIUM=/usr/bin/chromium node --test tests/peerjs_webrtc_browser_test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { chromiumPath, openChromiumPage } from "./support/chromium_page.mjs";
import { exchangeInPage, takeoverInPage } from "./support/peerjs_page_scenarios.mjs";
import { startPeerJsServer } from "./support/peerjs_server.mjs";

test("put, lookup, watch and host takeover over real data channels", { skip: !chromiumPath && "set P2PDC_TEST_CHROMIUM", timeout: 120000 },
     async (t) => {
         const server = await startPeerJsServer({ expireMs: 1000 });
         t.after(() => server.close());
         const page = await openChromiumPage("tests/peerjs_webrtc_page.html");
         t.after(() => page.close());
         const exchange = await page.evaluate(exchangeInPage, server.url, "peerjs", [], 10000);
         assert.deepEqual(exchange, { ready: true, found: true, heard: true, writerHosts: true, watcherConnected: true });
         const takeover = await page.evaluate(takeoverInPage, server.url, 10000);
         assert.equal(takeover.ready, true);
         assert.ok(takeover.heard.includes(3), `the watch heard the value put after the takeover: ${takeover.heard}`);
         assert.equal(takeover.watcherHosts, true);
         assert.deepEqual(takeover.values, [2, 3], "the writer put its values again on the new host");
         assert.equal(server.dropped, 0, "only genuine signalling went through the server");
     });
