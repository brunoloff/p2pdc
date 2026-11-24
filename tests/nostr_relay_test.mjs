import test from "node:test";
import assert from "node:assert/strict";
import { NostrRelay } from "../src/browser/nostr_relay.mjs";
import { isTransientProxyError, keyHex } from "../src/browser/proxy_codec.mjs";
import { storeLimits } from "../src/browser/store_limits.mjs";
import { startNostrRelay } from "./support/nostr_relay_server.mjs";

const key = (seed) => Uint8Array.from({ length: 20 }, (_, index) => (index + seed) % 256 || 1);
const limits = storeLimits({ timeoutMs: 2000 });
const bytes = (...values) => Uint8Array.from(values);
const until = async (check, ms = 2000) => {
    for (const end = Date.now() + ms; Date.now() < end; await new Promise((r) => setTimeout(r, 10))) if (check()) return;
    throw new Error("condition not met in time");
};

test("values put under a key come back from lookups, from every writer", async (t) => {
    const relay = await startNostrRelay(); t.after(() => relay.close());
    const alice = new NostrRelay(relay.url, limits), bob = new NostrRelay(relay.url, limits);
    t.after(() => { alice.close(); bob.close(); });
    await alice.put(key(1), bytes(1, 2, 3));
    await bob.put(key(1), bytes(4));
    await bob.put(key(2), bytes(9));
    const found = await alice.get(key(1));
    assert.deepEqual(found.map((record) => [...record.bytes]).sort(), [[1, 2, 3], [4]]);
    assert.ok(found.every((record) => record.expired === false && typeof record.id === "string"));
    assert.equal(relay.invalid, 0, "the relay accepted every id and signature");
    const event = relay.events[0];
    assert.equal(event.tags.find((entry) => entry[0] === "x")[1], keyHex(key(1)));
    const expires = Number(event.tags.find((entry) => entry[0] === "expiration")[1]);
    assert.ok(Math.abs(expires - (Date.now() / 1000 + 600)) < 5);
});

test("a watch hears of values put after it is ready", async (t) => {
    const relay = await startNostrRelay(); t.after(() => relay.close());
    const watcher = new NostrRelay(relay.url, limits), writer = new NostrRelay(relay.url, limits);
    t.after(() => { watcher.close(); writer.close(); });
    const heard = [];
    const watch = watcher.listen(key(3), (record) => heard.push([...record.bytes]));
    assert.equal(await watch.ready, true);
    await writer.put(key(3), bytes(7));
    await writer.put(key(4), bytes(8));
    await until(() => heard.length === 1);
    watch.close();
    await watch.done;
    await writer.put(key(3), bytes(5));
    await new Promise((r) => setTimeout(r, 50));
    assert.deepEqual(heard, [[7]]);
});

test("expired and malformed events are left out", async (t) => {
    const relay = await startNostrRelay(); t.after(() => relay.close());
    const store = new NostrRelay(relay.url, limits); t.after(() => store.close());
    await store.put(key(5), bytes(1));
    const stale = await store.event(key(5), bytes(2));
    stale.tags = stale.tags.map((entry) => entry[0] === "expiration" ? ["expiration", "1"] : entry);
    relay.events.push(stale, { ...stale, kind: 1, tags: [["x", keyHex(key(5))]] },
                      { ...stale, id: "junk", tags: [["x", keyHex(key(5))]], content: "not base64!" });
    assert.deepEqual((await store.get(key(5))).map((record) => [...record.bytes]), [[1]]);
});

test("refusals say whether trying again may help", async (t) => {
    const relay = await startNostrRelay(); t.after(() => relay.close());
    const store = new NostrRelay(relay.url, limits); t.after(() => store.close());
    relay.refuseNext = "rate-limited: slow down";
    const limited = await store.put(key(6), bytes(1)).catch((error) => error);
    assert.equal(isTransientProxyError(limited), true);
    relay.refuseNext = "blocked: not on the list";
    const blocked = await store.put(key(6), bytes(1)).catch((error) => error);
    assert.equal(blocked.code, "rejected");
    assert.equal(isTransientProxyError(blocked), false);
    relay.closeNextRequest = "auth-required: sign in";
    assert.equal((await store.get(key(6)).catch((error) => error)).code, "rejected");
    await assert.rejects(store.put(key(6), new Uint8Array(40000)), TypeError);
});

test("losing the relay ends watches with a transient error, and the store reconnects", async (t) => {
    const relay = await startNostrRelay(); t.after(() => relay.close());
    const store = new NostrRelay(relay.url, limits); t.after(() => store.close());
    const watch = store.listen(key(7), () => {});
    assert.equal(await watch.ready, true);
    relay.server.dropAll();
    const error = await watch.done.catch((failure) => failure);
    assert.equal(isTransientProxyError(error), true);
    await until(() => Date.now() >= store.channel.notBefore, 3000);
    await store.put(key(7), bytes(3));
    assert.deepEqual((await store.get(key(7))).map((record) => [...record.bytes]), [[3]]);
});

test("only wss:// relays, or ws:// on this computer, are accepted", () => {
    assert.throws(() => new NostrRelay("https://relay.example", limits), TypeError);
    assert.throws(() => new NostrRelay("ws://relay.example", limits), TypeError);
    assert.doesNotThrow(() => new NostrRelay("wss://relay.example", limits));
});
