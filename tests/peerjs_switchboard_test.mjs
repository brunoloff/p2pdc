import test from "node:test";
import assert from "node:assert/strict";
import { PeerJsSwitchboard } from "../src/browser/peerjs_switchboard.mjs";
import { isTransientProxyError, keyHex } from "../src/browser/proxy_codec.mjs";
import { storeLimits } from "../src/browser/store_limits.mjs";
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
    const server = await startPeerJsServer(options);
    const stores = [];
    const store = (serverKey = "peerjs") => { const s = new PeerJsSwitchboard(server.url, serverKey, limits); stores.push(s); return s; };
    t.after(async () => { for (const s of stores) s.close(); await server.close(); });
    return { server, store };
}

test("the first store to use a key hosts it, and others put and look up through it", async (t) => {
    const { server, store } = await setup(t);
    const alice = store(), bob = store();
    await alice.put(key(1), bytes(1, 2));
    assert.ok(alice.hosts.has(keyHex(key(1))), "alice hosts the key");
    await bob.put(key(1), bytes(3));
    assert.ok(bob.remote.has(keyHex(key(1))), "bob found the ID taken");
    assert.deepEqual(sorted(await bob.get(key(1))), [[1, 2], [3]]);
    assert.deepEqual(sorted(await alice.get(key(1))), [[1, 2], [3]]);
    assert.deepEqual(await bob.get(key(2)), [], "an unused key is empty");
    assert.ok(server.clients.has(`p2pdc${keyHex(key(1))}`));
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
});

test("when the host leaves, the next store takes over and writers put their values again", async (t) => {
    const { server, store } = await setup(t);
    const first = store(), second = store(), third = store();
    await first.put(key(4), bytes(1));
    await second.put(key(4), bytes(2));
    first.close();
    await until(() => !server.clients.has(`p2pdc${keyHex(key(4))}`));
    // The old host's ID expires a request; the asker then hosts the key itself.
    assert.deepEqual(sorted(await second.get(key(4))), [[2]], "second took over with its own value");
    assert.ok(second.hosts.has(keyHex(key(4))));
    await third.put(key(4), bytes(3));
    assert.deepEqual(sorted(await third.get(key(4))), [[2], [3]]);
    assert.ok(server.expired >= 1);
});

test("a remote watch moves to the new host", async (t) => {
    const { server, store } = await setup(t);
    const host = store(), watcher = store(), writer = store();
    await host.put(key(5), bytes(1));
    const heard = [];
    const watch = watcher.listen(key(5), (record) => heard.push([...record.bytes]));
    assert.equal(await watch.ready, true);
    host.close();
    await until(() => !server.clients.has(`p2pdc${keyHex(key(5))}`));
    await writer.put(key(5), bytes(2)); // writer becomes the host
    for (const sub of watcher.watches.values()) await sub.subscribe(); // as its 30-second renewal does
    await writer.put(key(5), bytes(3));
    await until(() => heard.some((value) => value[0] === 3));
    watch.close();
});

test("a wrong server key is refused for good; an unreachable server is transient", async (t) => {
    const { store } = await setup(t);
    const wrong = store("other");
    const refused = await wrong.put(key(6), bytes(1)).catch((error) => error);
    assert.equal(refused.code, "rejected");
    const nowhere = new PeerJsSwitchboard("ws://127.0.0.1:1/peerjs", "peerjs", limits);
    t.after(() => nowhere.close());
    assert.equal(isTransientProxyError(await nowhere.get(key(6)).catch((error) => error)), true);
    assert.throws(() => new PeerJsSwitchboard("https://0.peerjs.com/peerjs", "peerjs", limits), TypeError);
});

test("hosts cap what anyone may put", async (t) => {
    const { store } = await setup(t);
    const host = store(), flooder = store();
    await host.put(key(7), bytes(1));
    let full = null;
    for (let i = 0; i < 300 && !full; ++i) full = await flooder.put(key(7), bytes(i % 256)).then(() => null, (error) => error);
    assert.equal(full?.status, 429);
});
