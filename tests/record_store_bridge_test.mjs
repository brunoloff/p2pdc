import test from "node:test";
import assert from "node:assert/strict";
import { createRecordBridge } from "../src/browser/record_store_bridge.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

const key = Uint8Array.from({ length: 20 }, (_, index) => index + 1);
const deferred = () => {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
};
const settle = async () => { for (let index = 0; index < 8; ++index) await Promise.resolve(); };
function fixture() {
    const events = [], writes = [], gets = [], listeners = [];
    const proxy = {
        put: (k, value, signal) => { const done = deferred(); writes.push({ k, value, signal, done }); return done.promise; },
        get: (k, signal, options) => { const done = deferred(); gets.push({ k, signal, options, done }); return done.promise; },
        listen: (k, callback, signal) => {
            const ready = deferred(), done = deferred(), listener = { k, callback, signal, ready, done, closed: false };
            listeners.push(listener);
            return { ready: ready.promise, done: done.promise, close: () => { listener.closed = true; } };
        },
    };
    const bridge = createRecordBridge((...event) => { events.push(event); return true; }, () => proxy);
    return { bridge, events, writes, gets, listeners, id: bridge.create("https://proxy.example", {}) };
}

test("put and lookup copy inputs, preserve records and report failures", async () => {
    const f = fixture(), value = Uint8Array.of(0, 255);
    f.bridge.put(f.id, 1, key, value); value.fill(4);
    assert.deepEqual([...f.writes[0].value], [0, 255]);
    f.writes[0].done.resolve(); await settle();
    assert.deepEqual(f.events[0], [f.id, 1, 0, null, 1]);
    f.bridge.lookup(f.id, 2, key, true);
    assert.equal(f.gets[0].options.deduplicateByContent, true);
    f.gets[0].done.resolve([{ bytes: Uint8Array.of(1), expired: false }, { bytes: Uint8Array.of(2), expired: true }]);
    await settle();
    assert.deepEqual(f.events.slice(1).map((event) => [event[2], event[3] && [...event[3]], event[4]]),
        [[1, [1], 0], [2, null, 0]]);
    f.bridge.lookup(f.id, 3, key, false);
    f.gets[1].done.reject({ code: "resource_limit" }); await settle();
    assert.deepEqual(f.events.at(-1), [f.id, 3, 2, null, 3]);
    f.bridge.close(f.id);
});

test("watch reports registration, records, failures and suppresses events after cancellation", async () => {
    const f = fixture(); f.bridge.watch(f.id, 1, key);
    const watch = f.listeners[0]; watch.ready.resolve(true); await settle();
    watch.callback({ bytes: Uint8Array.of(7), expired: true });
    assert.deepEqual(f.events.map((event) => [event[2], event[4]]), [[3, 1], [4, 1]]);
    f.bridge.cancel(f.id, 1); assert.equal(watch.closed, true); assert.equal(watch.signal.aborted, true);
    watch.callback({ bytes: Uint8Array.of(8), expired: false });
    watch.done.reject(new Error("closed")); await settle(); assert.equal(f.events.length, 2);
    f.bridge.watch(f.id, 2, key); const failed = f.listeners[1];
    failed.done.reject({ code: "invalid_record" }); await settle();
    assert.deepEqual(f.events.at(-1), [f.id, 2, 5, null, 4]);
    assert.equal(failed.closed, true); f.bridge.close(f.id);
});

test("same-key watches share a stream but close independently", async () => {
    const f = fixture(), other = f.bridge.create("https://proxy.example", {});
    f.bridge.watch(f.id, 1, key); f.bridge.watch(other, 1, key);
    assert.equal(f.listeners.length, 1);
    const watch = f.listeners[0];
    watch.ready.resolve(true); await settle();
    assert.deepEqual(f.events.map((event) => [event[0], event[2], event[4]]),
        [[f.id, 3, 1], [other, 3, 1]]);
    f.bridge.cancel(f.id, 1);
    assert.equal(watch.closed, false); assert.equal(watch.signal.aborted, false);
    watch.callback({ bytes: Uint8Array.of(9), expired: false });
    assert.deepEqual(f.events.at(-1), [other, 1, 4, Uint8Array.of(9), 0]);
    f.bridge.close(other);
    assert.equal(watch.closed, true); assert.equal(watch.signal.aborted, true);
    f.bridge.close(f.id);
});

test("shared stream failures reach each active watcher and different limits remain separate", async () => {
    const f = fixture(), other = f.bridge.create("https://proxy.example", {});
    const bounded = f.bridge.create("https://proxy.example", { maxValueBytes: 512 });
    f.bridge.watch(f.id, 1, key); f.bridge.watch(other, 1, key); f.bridge.watch(bounded, 1, key);
    assert.equal(f.listeners.length, 2);
    f.listeners[0].done.reject({ code: "invalid_record" }); await settle();
    assert.deepEqual(f.events.map((event) => [event[0], event[2], event[4]]),
        [[f.id, 5, 4], [other, 5, 4]]);
    assert.equal(f.listeners[0].closed, true);
    assert.equal(f.listeners[1].closed, false);
    f.bridge.close(f.id); f.bridge.close(other); f.bridge.close(bounded);
});

test("invalid operations fail early and close aborts outstanding work", () => {
    const f = fixture();
    assert.throws(() => f.bridge.put(f.id, 1, new Uint8Array(20), Uint8Array.of(1)), TypeError);
    f.bridge.put(f.id, 1, key, Uint8Array.of(1));
    assert.throws(() => f.bridge.lookup(f.id, 1, key, false), TypeError);
    f.bridge.close(f.id); assert.equal(f.writes[0].signal.aborted, true);
    assert.throws(() => f.bridge.watch(f.id, 2, key), TypeError);
});

test("canceled lookup rejection is silent and does not deliver events", async () => {
    const f = fixture(), previous = console.error, errors = [];
    console.error = (...args) => errors.push(args);
    try {
        f.bridge.lookup(f.id, 1, key, false);
        f.bridge.cancel(f.id, 1);
        f.gets[0].done.reject(new Error("aborted"));
        await settle();
        assert.deepEqual(f.events, []); assert.deepEqual(errors, []);
    } finally {
        console.error = previous; f.bridge.close(f.id);
    }
});

test("failed registration and rejected delivery close the watch", async () => {
    const f = fixture(); f.bridge.watch(f.id, 1, key);
    f.listeners[0].ready.resolve(false); await settle();
    assert.deepEqual(f.events, [[f.id, 1, 3, null, 0]]);
    assert.equal(f.listeners[0].closed, true);
    f.bridge.close(f.id);
    const proxy = { listen: (_key, callback) => {
        const ready = deferred(), done = deferred();
        queueMicrotask(() => callback({ bytes: Uint8Array.of(1), expired: false }));
        return { ready: ready.promise, done: done.promise, close: () => { proxy.closed = true; } };
    } };
    const bridge = createRecordBridge(() => false, () => proxy), id = bridge.create("https://proxy.example", {});
    bridge.watch(id, 1, key); await settle();
    assert.equal(proxy.closed, true); bridge.close(id);
});

test("proxy access rejection is permanent and startup reports its error", async () => {
    const f = fixture(); f.bridge.watch(f.id, 1, key);
    const denied = new ProxyError("http_error", "denied"); denied.status = 403;
    f.listeners[0].ready.resolve(false); f.listeners[0].done.reject(denied); await settle();
    assert.deepEqual(f.events, [[f.id, 1, 5, null, 6]]);
    assert.equal(f.listeners[0].closed, true);
    f.bridge.lookup(f.id, 2, key, false);
    f.gets[0].done.reject(denied); await settle();
    assert.deepEqual(f.events.at(-1), [f.id, 2, 2, null, 6]);
    f.bridge.close(f.id);
});

test("temporary proxy errors remain retryable", async () => {
    const f = fixture(); f.bridge.watch(f.id, 1, key);
    f.listeners[0].ready.resolve(true); await settle();
    f.listeners[0].done.reject(new ProxyError("network_error", "offline")); await settle();
    assert.deepEqual(f.events.at(-1), [f.id, 1, 5, null, 2]);
    f.bridge.close(f.id);
});

test("operation count is bounded and synchronous backend failures release their slot", () => {
    const never = new Promise(() => {}), proxy = { put: () => never };
    const bridge = createRecordBridge(() => true, () => proxy), id = bridge.create("https://proxy.example", {});
    for (let index = 1; index <= 128; ++index) bridge.put(id, index, key, Uint8Array.of(1));
    assert.throws(() => bridge.put(id, 129, key, Uint8Array.of(1)), TypeError);
    bridge.cancel(id, 1); bridge.put(id, 129, key, Uint8Array.of(1)); bridge.close(id);
    const broken = createRecordBridge(() => true, () => ({ put: () => { throw new Error("broken"); } }));
    const other = broken.create("https://proxy.example", {});
    assert.throws(() => broken.put(other, 1, key, Uint8Array.of(1)), /broken/);
    assert.throws(() => broken.put(other, 1, key, Uint8Array.of(1)), /broken/);
    broken.close(other);
});

test("a store over several proxies takes them newline-separated", () => {
    const bridge = createRecordBridge(() => true);
    const id = bridge.create("https://one.example\nhttps://two.example", {});
    assert.equal(bridge.stores.get(id).origin, "https://one.example");
    assert.equal(bridge.stores.get(id).proxy.proxies.length, 2);
    assert.throws(() => bridge.create("https://one.example\nftp://two.example", {}), TypeError);
});
