import test from "node:test";
import assert from "node:assert/strict";
import { RecoveringMailbox } from "../src/browser/recovering_mailbox.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
function fixture(options = {}) {
    const streams = [], lookups = [], lookupOptions = [], timers = new Map(), events = [];
    let next = 0;
    const proxy = {
        listen(_key, callback) {
            const done = deferred(); streams.push({ done, callback });
            return { ready: Promise.resolve(true), done: done.promise, close: () => done.resolve() };
        },
        get(_key, _signal, options) { const item = deferred(); lookupOptions.push(options); lookups.push(item); return item.promise; },
    };
    const clock = { set: (callback, delay) => { const id = ++next; timers.set(id, { callback, delay }); return id; },
        clear: (id) => timers.delete(id) };
    const handlers = { onRecord: (value) => events.push(["record", [...value]]),
        onCatchup: () => events.push(["catchup"]), onError: (error) => events.push(["error", error.code]),
        onStatus: (status) => events.push(["status", status]) };
    const mailbox = new RecoveringMailbox(proxy, new Uint8Array(20).fill(1), handlers, clock, options);
    function fire() { const [id, timer] = timers.entries().next().value; timers.delete(id); timer.callback(); }
    return { mailbox, streams, lookups, lookupOptions, timers, events, fire };
}
async function settle() { for (let i = 0; i < 30; ++i) await Promise.resolve(); }
async function start(f) {
    const started = f.mailbox.start(); await settle(); f.lookups[0].resolve([]);
    assert.equal(await started, true); await settle();
}
const record = (value) => ({ bytes: Uint8Array.of(value), expired: false });
const offline = () => new ProxyError("stream_closed", "stream ended");

test("recovery reopens the stream, catches up and reports availability", async () => {
    const f = fixture(); await start(f);
    f.streams[0].done.reject(offline()); await settle();
    assert.equal(f.mailbox.healthy(), false);
    assert.deepEqual(f.events, [["catchup"], ["status", "reconnecting"]]);
    f.fire(); await settle();
    f.streams[1].callback(record(1)); await settle();
    assert.equal(f.events.length, 2);
    f.lookups[1].resolve([record(2)]); await settle();
    assert.equal(f.mailbox.healthy(), true);
    assert.deepEqual(f.lookupOptions, [{ deduplicateByContent: true }, { deduplicateByContent: true }]);
    assert.deepEqual(f.events, [["catchup"], ["status", "reconnecting"], ["status", "ready"],
        ["record", [1]], ["record", [2]], ["catchup"]]);
    f.mailbox.close();
});

test("retry exhaustion fails once and close cancels a pending retry", async () => {
    const f = fixture({ maxRetries: 1, retryDelayMs: 50 }); await start(f);
    f.streams[0].done.reject(offline()); await settle(); f.fire(); await settle();
    f.lookups[1].reject(offline()); await settle();
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "stream_closed"]]);
    assert.equal(f.mailbox.failed, true); assert.equal(f.timers.size, 0);
    const g = fixture(); await start(g); g.streams[0].done.reject(offline()); await settle();
    assert.equal(g.timers.size, 1);
    const late = g.timers.values().next().value.callback;
    g.mailbox.close(); assert.equal(g.timers.size, 0);
    late(); await settle(); assert.equal(g.streams.length, 1);
});

test("discovery startup buffers one copy even across separate deliveries", async () => {
    const f = fixture(), started = f.mailbox.start(); await settle();
    for (let i = 0; i < 150; ++i) { f.streams[0].callback(record(1)); await settle(); }
    f.streams[0].callback(record(2)); await settle(); f.lookups[0].resolve([]);
    assert.equal(await started, true); await settle();
    assert.deepEqual(f.events, [["record", [1]], ["record", [2]], ["catchup"]]);
    f.mailbox.close();
});

test("large startup buffers drain in bounded batches", async () => {
    const f = fixture(), started = f.mailbox.start(); await settle();
    let pending = 0, highest = 0, delivered = 0;
    f.mailbox.handlers.onRecord = () => {
        ++delivered; highest = Math.max(highest, ++pending); queueMicrotask(() => --pending);
    };
    for (let index = 0; index < 200; ++index) {
        f.streams[0].callback({ bytes: Uint8Array.of(index >> 8, index & 255), expired: false });
        await settle();
    }
    f.lookups[0].resolve([]);
    assert.equal(await started, true); await settle();
    assert.equal(delivered, 200); assert.ok(highest <= 16);
    assert.deepEqual(f.events, [["catchup"]]); f.mailbox.close();
});

test("closing during a buffered batch stops later delivery", async () => {
    const f = fixture(), started = f.mailbox.start(); await settle();
    let delivered = 0;
    f.mailbox.handlers.onRecord = () => {
        if (++delivered === 1) queueMicrotask(() => f.mailbox.close());
    };
    for (let index = 0; index < 40; ++index) {
        f.streams[0].callback({ bytes: Uint8Array.of(index), expired: false });
        await settle();
    }
    f.lookups[0].resolve([]);
    assert.equal(await started, false); await settle();
    assert.equal(delivered, 16); assert.deepEqual(f.events, []);
});

test("startup buffer rejects a 513th distinct record", async () => {
    const f = fixture(), started = f.mailbox.start(); await settle();
    for (let index = 0; index < 513; ++index) {
        f.streams[0].callback({ bytes: Uint8Array.of(index >> 8, index & 255), expired: false });
        await settle();
    }
    f.lookups[0].resolve([]);
    assert.equal(await started, false); await settle();
    assert.equal(f.mailbox.failed, true);
    assert.equal(f.events.filter((event) => event[0] === "error").length, 1);
    f.mailbox.close();
});

test("rejects invalid recovery configuration and reports initial failure", async () => {
    assert.throws(() => fixture({ maxRetries: -1 }), TypeError);
    assert.throws(() => fixture({ retryDelayMs: 0 }), TypeError);
    const f = fixture(); const pending = f.mailbox.start(); await settle();
    f.lookups[0].reject(offline()); assert.equal(await pending, false); await settle();
    assert.equal(f.mailbox.failed, true); assert.equal(f.timers.size, 0);
});

test("invalid data is terminal and a late failed attempt cannot revive a closed mailbox", async () => {
    const f = fixture(); await start(f);
    f.streams[0].done.reject(new ProxyError("invalid_record", "bad record")); await settle();
    assert.equal(f.mailbox.failed, true); assert.equal(f.timers.size, 0);
    const g = fixture(); await start(g); g.streams[0].done.reject(offline()); await settle();
    g.fire(); await settle(); g.mailbox.close(); g.lookups[1].resolve([record(3)]); await settle();
    assert.equal(g.mailbox.healthy(), false);
    assert.equal(g.events.some((event) => event[0] === "record"), false);
});

test("temporary HTTP failures retry while rejected access fails immediately", async () => {
    const retry = fixture(); await start(retry);
    const unavailable = new ProxyError("http_error", "unavailable"); unavailable.status = 503;
    retry.streams[0].done.reject(unavailable); await settle();
    assert.equal(retry.timers.size, 1); retry.mailbox.close();
    const deny = fixture(); await start(deny);
    const forbidden = new ProxyError("http_error", "forbidden"); forbidden.status = 403;
    deny.streams[0].done.reject(forbidden); await settle();
    assert.equal(deny.mailbox.failed, true); assert.equal(deny.timers.size, 0);
});
