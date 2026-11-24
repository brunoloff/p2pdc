// Opt-in tests against public servers, one test per server named in these variables
// (space-separated): P2PDC_TEST_NOSTR_RELAYS, P2PDC_TEST_MQTT_BROKERS, P2PDC_TEST_PEERJS_SERVERS
// ("wss://host/path" or "wss://host/path#key", the key defaulting to "peerjs"). PeerJS needs
// WebRTC, so its tests run in headless Chromium, named by P2PDC_TEST_CHROMIUM.
//   P2PDC_TEST_NOSTR_RELAYS="wss://nos.lol" node --test tests/discovery_backends_live_test.mjs
//   P2PDC_TEST_CHROMIUM=/usr/bin/chromium P2PDC_TEST_PEERJS_SERVERS="wss://0.peerjs.com/peerjs" \
//       node --test tests/discovery_backends_live_test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { MqttBroker } from "../src/browser/mqtt_broker.mjs";
import { NostrRelay } from "../src/browser/nostr_relay.mjs";
import { storeLimits } from "../src/browser/store_limits.mjs";
import { openChromiumPage } from "./support/chromium_page.mjs";
import { exchangeInPage } from "./support/peerjs_page_scenarios.mjs";

const limits = storeLimits({ timeoutMs: 15000 });
const servers = (name) => (process.env[name] ?? "").split(/\s+/).filter(Boolean);
const kinds = [["Nostr relay", "P2PDC_TEST_NOSTR_RELAYS", (url) => new NostrRelay(url, limits)],
               ["MQTT broker", "P2PDC_TEST_MQTT_BROKERS", (url) => new MqttBroker(url, limits)]];

for (const [kind, variable, create] of kinds) {
    for (const url of servers(variable)) {
        test(`${kind} ${url} keeps, finds and pushes values between two clients`, { timeout: 90000 }, async () => {
            const key = crypto.getRandomValues(new Uint8Array(20)), first = crypto.getRandomValues(new Uint8Array(300));
            const second = crypto.getRandomValues(new Uint8Array(300));
            const writer = create(url), watcher = create(url);
            try {
                await writer.put(key, first);
                const heard = [];
                const watch = watcher.listen(key, (record) => heard.push([...record.bytes]));
                watch.done.catch(() => {}); // a failed watch shows as ready being false
                assert.equal(await watch.ready, true, "the watch is ready");
                assert.deepEqual((await watcher.get(key)).map((record) => [...record.bytes]), [[...first]]);
                await writer.put(key, second);
                const isSecond = (value) => value.length === second.length && value.every((byte, i) => byte === second[i]);
                for (let i = 0; i < 150 && !heard.some(isSecond); ++i) await new Promise((r) => setTimeout(r, 100));
                // Some servers also replay values from just before the watch began.
                assert.ok(heard.some(isSecond), "the watch heard the new value");
                watch.close();
            } finally { writer.close(); watcher.close(); }
        });
    }
}

// Through the server's WebRTC signalling and PeerJS's default STUN and TURN servers.
for (const spec of servers("P2PDC_TEST_PEERJS_SERVERS")) {
    test(`PeerJS server ${spec} keeps, finds and pushes values between two clients`, { timeout: 120000 }, async (t) => {
        const [url, key = "peerjs"] = spec.split("#");
        const page = await openChromiumPage("tests/peerjs_webrtc_page.html");
        t.after(() => page.close());
        const result = await page.evaluate(exchangeInPage, url, key, null, limits.timeoutMs);
        assert.deepEqual(result, { ready: true, found: true, heard: true, writerHosts: true, watcherConnected: true });
    });
}
