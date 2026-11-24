import test from "node:test";
import assert from "node:assert/strict";
import { createBackendStore } from "../src/browser/discovery_backends.mjs";
import { FailoverProxy } from "../src/browser/failover_proxy.mjs";
import { FanOutStore } from "../src/browser/fan_out_store.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { startNostrRelay } from "./support/nostr_relay_server.mjs";

const json = (...entries) => JSON.stringify(entries);
const key = Uint8Array.from({ length: 20 }, (_, index) => index + 1);

test("one backend is used as it is; several are used all at once", () => {
    const single = createBackendStore(json({ kind: "opendht", urls: ["https://one.example"] }));
    assert.ok(single instanceof OpenDhtProxy);
    assert.equal(single.streamOrigin, "https://one.example");
    const failover = createBackendStore(json({ kind: "opendht", urls: ["https://one.example", "https://two.example"] }));
    assert.ok(failover instanceof FailoverProxy);
    const all = createBackendStore(json({ kind: "opendht", urls: ["https://one.example"] },
                                        { kind: "nostr", url: "wss://relay.example" },
                                        { kind: "mqtt", url: "wss://broker.example/mqtt" },
                                        { kind: "peerjs", url: "wss://peers.example/peerjs", key: "peerjs" }));
    assert.ok(all instanceof FanOutStore);
    assert.equal(all.backends.length, 4);
    assert.equal(all.streamOrigin, "https://one.example", "streaming HTTP watches keep their budget");
    all.close();
});

test("malformed lists and entries are refused", () => {
    for (const text of ["not json", "[]", json({ kind: "smoke", url: "wss://x.example" }), json({ kind: "nostr" }),
                        json({ kind: "nostr", url: "https://relay.example" }), json({ kind: "opendht", urls: [] }),
                        json({ kind: "peerjs", url: "wss://p.example" }), json(...Array(9).fill({ kind: "nostr", url: "wss://r.example" }))])
        assert.throws(() => createBackendStore(text), TypeError, text);
});

test("stores on the same server share one connection, closed with the last store", async (t) => {
    const relay = await startNostrRelay(); t.after(() => relay.close());
    const list = json({ kind: "nostr", url: relay.url });
    const first = createBackendStore(list, { timeoutMs: 2000 }), second = createBackendStore(list, { timeoutMs: 2000, maxValues: 4 });
    await first.put(key, Uint8Array.of(1));
    assert.deepEqual((await second.get(key)).map((value) => [...value.bytes]), [[1]]);
    assert.equal(relay.server.connections.size, 1);
    first.close();
    assert.deepEqual((await second.get(key)).map((value) => [...value.bytes]), [[1]], "still open for the other store");
    second.close();
    for (let i = 0; i < 100 && relay.server.connections.size; ++i) await new Promise((r) => setTimeout(r, 10));
    assert.equal(relay.server.connections.size, 0);
});
