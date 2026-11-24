import test from "node:test";
import assert from "node:assert/strict";
import { MqttBroker } from "../src/browser/mqtt_broker.mjs";
import { PacketDecoder, publishPacket } from "../src/browser/mqtt_codec.mjs";
import { isTransientProxyError, keyHex } from "../src/browser/proxy_codec.mjs";
import { storeLimits } from "../src/browser/store_limits.mjs";
import { startMqttBroker } from "./support/mqtt_broker_server.mjs";

const key = (seed) => Uint8Array.from({ length: 20 }, (_, index) => (index + seed) % 256 || 1);
const limits = storeLimits({ timeoutMs: 2000 });
const bytes = (...values) => Uint8Array.from(values);
const sorted = (records) => records.map((record) => [...record.bytes]).sort();
const until = async (check, ms = 2000) => {
    for (const end = Date.now() + ms; Date.now() < end; await new Promise((r) => setTimeout(r, 10))) if (check()) return;
    throw new Error("condition not met in time");
};
async function setup(t) {
    const broker = await startMqttBroker();
    const clients = [];
    const client = () => { const store = new MqttBroker(broker.url, limits); clients.push(store); return store; };
    t.after(async () => { for (const store of clients) store.close(); await broker.close(); });
    return { broker, client };
}

test("values put under a key come back from lookups, from every writer", async (t) => {
    const { broker, client } = await setup(t);
    const alice = client(), bob = client();
    await alice.put(key(1), bytes(1, 2));
    await bob.put(key(1), bytes(3));
    await bob.put(key(2), bytes(9));
    assert.deepEqual(sorted(await alice.get(key(1))), [[1, 2], [3]]);
    assert.deepEqual(broker.protocolLevels, [5, 5], "MQTT 5 connections");
    const [topic, value] = [...broker.retained].find(([name]) => name.startsWith(`p2pdc/v1/${keyHex(key(1))}/`));
    assert.match(topic, /^p2pdc\/v1\/[0-9a-f]{40}\/[0-9a-f]{32}$/);
    assert.ok(Math.abs(value.expires - (Date.now() + 600000)) < 5000, "expires after ten minutes");
});

test("expired messages are not replayed", async (t) => {
    const { broker, client } = await setup(t);
    const store = client();
    await store.put(key(3), bytes(1));
    for (const value of broker.retained.values()) value.expires = Date.now() - 1;
    await store.put(key(3), bytes(2));
    assert.deepEqual(sorted(await store.get(key(3))), [[2]]);
});

test("a watch hears values put after it is ready, and lookups meanwhile share its subscription", async (t) => {
    const { client } = await setup(t);
    const watcher = client(), writer = client();
    await writer.put(key(4), bytes(1));
    const heard = [];
    const watch = watcher.listen(key(4), (record) => heard.push([...record.bytes]));
    assert.equal(await watch.ready, true);
    assert.deepEqual(sorted(await watcher.get(key(4))), [[1]], "a lookup while watching");
    await writer.put(key(4), bytes(2));
    await until(() => heard.length === 1);
    assert.deepEqual(heard, [[2]], "the stored value went only to the lookup");
    watch.close();
    await watch.done;
    await writer.put(key(4), bytes(3));
    await new Promise((r) => setTimeout(r, 50));
    assert.deepEqual(heard, [[2]]);
    assert.equal(watcher.keys.size, 0, "unsubscribed once nobody uses the key");
});

test("refusals say whether trying again may help", async (t) => {
    const { broker, client } = await setup(t);
    const store = client();
    broker.refuseNextPublish = 0x97; // quota exceeded
    assert.equal(isTransientProxyError(await store.put(key(5), bytes(1)).catch((error) => error)), true);
    broker.refuseNextPublish = 0x87; // not authorized
    const refused = await store.put(key(5), bytes(1)).catch((error) => error);
    assert.equal(refused.code, "rejected");
    broker.refuseNextSubscribe = 0x87;
    const watch = store.listen(key(5), () => {});
    assert.equal(await watch.ready, false);
    assert.equal((await watch.done.catch((error) => error)).code, "rejected");
    await assert.rejects(store.put(key(5), new Uint8Array(40000)), TypeError);
});

test("losing the broker ends watches with a transient error, and the store reconnects", async (t) => {
    const { broker, client } = await setup(t);
    const store = client();
    const watch = store.listen(key(6), () => {});
    assert.equal(await watch.ready, true);
    broker.server.dropAll();
    assert.equal(isTransientProxyError(await watch.done.catch((error) => error)), true);
    await until(() => Date.now() >= store.channel.notBefore, 3000);
    await store.put(key(6), bytes(4));
    assert.deepEqual(sorted(await store.get(key(6))), [[4]]);
    assert.equal(broker.connects, 2);
});

test("the packet decoder reassembles packets split across messages", () => {
    const whole = publishPacket("p2pdc/v1/aa/bb", 7, new Uint8Array(300).fill(5), 600);
    const decoder = new PacketDecoder(4096);
    assert.deepEqual(decoder.push(whole.subarray(0, 2)), []);
    const [decoded] = decoder.push(whole.subarray(2));
    assert.equal(decoded.type, 3);
    assert.equal(decoded.body.length, whole.length - 3);
});
