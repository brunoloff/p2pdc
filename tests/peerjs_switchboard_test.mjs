import test from "node:test";
import assert from "node:assert/strict";
import { PeerJsSwitchboard } from "../src/browser/peerjs_switchboard.mjs";
import { isTransientProxyError, keyHex } from "../src/browser/proxy_codec.mjs";
import { storeLimits } from "../src/browser/store_limits.mjs";
import { fakeWebRtc } from "./support/fake_webrtc.mjs";
import { startPeerJsServer } from "./support/peerjs_server.mjs";

const key = (seed) => Uint8Array.from({ length: 20 }, (_, index) => (index + seed) % 256 || 1);
const limits = storeLimits({ timeoutMs: 2000 });
const bytes = (...values) => Uint8Array.from(values);
const sorted = (records) => records.map((record) => [...record.bytes]).sort();
const until = async (check, ms = 3000) => {
    for (const end = Date.now() + ms; Date.now() < end; await new Promise((r) => setTimeout(r, 10))) if (check()) return;
    throw new Error("condition not met in time");
};
async function setup(t, options) {
    const server = await startPeerJsServer(options), rtc = fakeWebRtc();
    const stores = [];
    const store = (serverKey = "peerjs") => { const s = new PeerJsSwitchboard(server.url, serverKey, limits, rtc); stores.push(s); return s; };
    t.after(async () => { for (const s of stores) s.close(); await server.close(); });
    return { server, store, rtc };
}

test("the first store to use a key hosts it, and others reach it over a WebRTC data connection", async (t) => {
    const { server, store, rtc } = await setup(t);
    const alice = store(), bob = store();
    await alice.put(key(1), bytes(1, 2));
    assert.ok(alice.hosts.has(keyHex(key(1))), "alice hosts the key");
    await bob.put(key(1), bytes(3));
    assert.ok(bob.remote.has(keyHex(key(1))), "bob found the ID taken");
    assert.deepEqual(sorted(await bob.get(key(1))), [[1, 2], [3]]);
    assert.deepEqual(sorted(await alice.get(key(1))), [[1, 2], [3]]);
    assert.ok(server.clients.has(`p2pdc${keyHex(key(1))}`));
    assert.equal(server.dropped, 0, "the server relayed everything: it was all genuine signalling");
    assert.deepEqual([...server.types.keys()].sort(), ["ANSWER", "CANDIDATE", "OFFER"]);
    assert.equal(rtc.connections.length, 2, "one peer connection on each side");
});

test("every request between a client and a host shares one data connection", async (t) => {
    const { server, store, rtc } = await setup(t);
    const host = store(), client = store();
    await host.put(key(2), bytes(1));
    await Promise.all([client.put(key(2), bytes(2)), client.get(key(2)), client.listen(key(2), () => {}).ready]);
    for (let i = 0; i < 5; ++i) await client.get(key(2));
    assert.equal(rtc.connections.length, 2);
    assert.equal(server.types.get("OFFER"), 1);
    assert.deepEqual(await client.get(key(9)), [], "an unused key is empty");
});

test("watches hear new values, on the host and elsewhere", async (t) => {
    const { store } = await setup(t);
    const host = store(), watcher = store(), writer = store();
    await host.put(key(3), bytes(1));
    const remote = [], local = [];
    const remoteWatch = watcher.listen(key(3), (record) => remote.push([...record.bytes]));
    const localWatch = host.listen(key(3), (record) => local.push([...record.bytes]));
    assert.equal(await remoteWatch.ready, true);
    assert.equal(await localWatch.ready, true);
    await writer.put(key(3), bytes(2));
    await until(() => remote.length === 1 && local.length === 1);
    assert.deepEqual([remote, local], [[[2]], [[2]]]);
    remoteWatch.close(); localWatch.close();
    await Promise.all([remoteWatch.done, localWatch.done]);
    await until(() => host.hosts.get(keyHex(key(3))).host.watchers.size === 0);
});

test("when the host leaves, the next store takes over and writers put their values again", async (t) => {
    const { server, store } = await setup(t);
    const first = store(), second = store(), third = store();
    await first.put(key(4), bytes(1));
    await second.put(key(4), bytes(2));
    first.close();
    await until(() => second.hosts.has(keyHex(key(4))));
    assert.deepEqual(sorted(await second.get(key(4))), [[2]], "second took over at once, with its own value");
    assert.ok(server.clients.has(`p2pdc${keyHex(key(4))}`));
    await third.put(key(4), bytes(3));
    assert.deepEqual(sorted(await third.get(key(4))), [[2], [3]]);
});

test("writers put their values again on the new host by themselves", async (t) => {
    const { store } = await setup(t);
    const host = store(), writer = store(), watcher = store();
    await host.put(key(17), bytes(1));
    await writer.put(key(17), bytes(2));
    const watch = watcher.listen(key(17), () => {});
    assert.equal(await watch.ready, true);
    host.close();
    await until(() => watcher.hosts.get(keyHex(key(17)))?.host.values().length === 1 ||
                      writer.hosts.get(keyHex(key(17)))?.host.values().length === 1);
    assert.deepEqual(sorted(await watcher.get(key(17))), [[2]], "the writer's value is back without a new put");
    watch.close();
});

test("an offer to a host that is gone expires, and the asker hosts the key itself", async (t) => {
    const { server, store } = await setup(t);
    const asker = store();
    asker.remote.add(keyHex(key(10))); // as if it had seen the ID taken a moment ago
    assert.deepEqual(await asker.get(key(10)), []);
    assert.ok(server.expired >= 1, "the server expired the offer");
    assert.ok(asker.hosts.has(keyHex(key(10))));
});

test("a remote watch moves to the new host by itself", async (t) => {
    const { store } = await setup(t);
    const host = store(), watcher = store(), writer = store();
    await host.put(key(5), bytes(1));
    const heard = [];
    const watch = watcher.listen(key(5), (record) => heard.push([...record.bytes]));
    assert.equal(await watch.ready, true);
    host.close();
    await until(() => watcher.hosts.has(keyHex(key(5))));
    await writer.put(key(5), bytes(3));
    await until(() => heard.some((value) => value[0] === 3));
    watch.close();
});

test("open data connections outlive the server connection", async (t) => {
    const { server, store } = await setup(t);
    const host = store(), client = store();
    await host.put(key(11), bytes(1));
    assert.equal((await client.get(key(11))).length, 1);
    server.drop(client.client.socket.id);
    await until(() => !server.clients.has(client.client.socket.id));
    assert.deepEqual(sorted(await client.get(key(11))), [[1]]);
});

test("a broken path ends the connection and the request moves on", async (t) => {
    const { store, rtc } = await setup(t);
    const host = store(), client = store();
    await host.put(key(12), bytes(1));
    await client.get(key(12));
    rtc.connections[0].breakPath();
    await until(() => !client.links.has(keyHex(key(12))));
    assert.deepEqual(sorted(await client.get(key(12))), [[1]], "connected again to the same host");
    assert.equal(rtc.connections.length, 4);
});

test("candidates that overtake the answer wait for it", async (t) => {
    const { store } = await setup(t, { delayAnswersMs: 50 });
    const host = store(), client = store();
    await host.put(key(13), bytes(1));
    assert.deepEqual(sorted(await client.get(key(13))), [[1]]);
});

test("lookups longer than one data-channel message arrive whole", async (t) => {
    const { store } = await setup(t);
    const host = store(), client = store();
    const values = Array.from({ length: 20 }, (_, i) => new Uint8Array(30000).fill(i));
    for (const value of values) await host.put(key(14), value);
    const found = await client.get(key(14));
    assert.deepEqual(found.map((record) => record.bytes[0]).sort((a, b) => a - b), values.map((_, i) => i));
    assert.ok(host.hosts.get(keyHex(key(14))).links.values().next().value.channel.sent > 20, "the reply was fragmented");
});

test("aborted requests reject at once", async (t) => {
    const { store } = await setup(t);
    const host = store(), client = store();
    await host.put(key(15), bytes(1));
    const controller = new AbortController();
    controller.abort();
    assert.equal((await client.get(key(15), controller.signal).catch((error) => error)).name, "AbortError");
});

test("a wrong server key is refused for good; an unreachable server is transient", async (t) => {
    const { store } = await setup(t);
    const wrong = store("other");
    const refused = await wrong.put(key(6), bytes(1)).catch((error) => error);
    assert.equal(refused.code, "rejected");
    const nowhere = new PeerJsSwitchboard("ws://127.0.0.1:1/peerjs", "peerjs", limits, fakeWebRtc());
    t.after(() => nowhere.close());
    assert.equal(isTransientProxyError(await nowhere.get(key(6)).catch((error) => error)), true);
    assert.throws(() => new PeerJsSwitchboard("https://0.peerjs.com/peerjs", "peerjs", limits), TypeError);
});

test("without WebRTC, as in Node, the backend refuses for good", async (t) => {
    const { server } = await setup(t);
    const store = new PeerJsSwitchboard(server.url, "peerjs", limits);
    t.after(() => store.close());
    const error = await store.put(key(16), bytes(1)).catch((caught) => caught);
    assert.equal(error.code, "rejected");
    assert.equal(isTransientProxyError(error), false);
});

test("hosts cap what anyone may put", async (t) => {
    const { store } = await setup(t);
    const host = store(), flooder = store();
    await host.put(key(7), bytes(1));
    let full = null;
    for (let i = 0; i < 300 && !full; ++i) full = await flooder.put(key(7), bytes(i % 256)).then(() => null, (error) => error);
    assert.equal(full?.status, 429);
});
