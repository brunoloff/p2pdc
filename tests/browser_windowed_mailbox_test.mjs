import test from "node:test";
import assert from "node:assert/strict";
import { WindowedMailbox, discoveryWindows } from "../src/browser/windowed_mailbox.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
function fixture(options = {}) {
    const streams = [], lookups = [], timers = new Map(), events = [];
    let nextTimer = 0;
    const proxy = {
        listen(key, callback) {
            const done = deferred(), stream = { window: key[0] - 1, callback, done, closed: false };
            streams.push(stream);
            return { ready: Promise.resolve(true), done: done.promise,
                close: () => { stream.closed = true; done.resolve(); } };
        },
        get(key) { const item = deferred(); lookups.push({ window: key[0] - 1, ...item }); return item.promise; },
    };
    const timersApi = { set: (callback) => { const id = ++nextTimer; timers.set(id, callback); return id; },
        clear: (id) => timers.delete(id) };
    const discovery = { windowKey: (time) => new Uint8Array(20).fill(Math.floor(time / 120) + 1) };
    const handlers = { onRecord: (wire) => events.push(["record", ...wire]),
        onCatchup: () => events.push(["catchup"]), onError: (error) => events.push(["error", error.message]),
        onStatus: (status) => events.push(["status", status]) };
    const mailbox = new WindowedMailbox(proxy, discovery, handlers, timersApi, options);
    const fire = () => { const [id, callback] = timers.entries().next().value; timers.delete(id); callback(); };
    return { mailbox, streams, lookups, timers, events, fire };
}
async function settle() { for (let i = 0; i < 30; ++i) await Promise.resolve(); }
async function ready(f, now = 1000) {
    const started = f.mailbox.start(now); await settle();
    for (const item of f.lookups) item.resolve([]);
    assert.equal(await started, true); await settle();
}

test("window ranges match the accepted time policy, including epoch zero", () => {
    assert.deepEqual(discoveryWindows(0), { first: 0, last: 0 });
    assert.deepEqual(discoveryWindows(1000), { first: 5, last: 8 });
    assert.deepEqual(discoveryWindows(1050), { first: 6, last: 9 });
    assert.throws(() => discoveryWindows(-1), RangeError);
    assert.throws(() => discoveryWindows(Number.MAX_SAFE_INTEGER), RangeError);
});

test("catch-up waits for every window and rollover closes old subscriptions", async () => {
    const f = fixture(), started = f.mailbox.start(1000); await settle();
    assert.deepEqual(f.lookups.map((item) => item.window), [5, 6, 7, 8]);
    for (const item of f.lookups.slice(0, 3)) item.resolve([]);
    await settle(); assert.deepEqual(f.events, []);
    f.lookups[3].resolve([{ bytes: Uint8Array.of(9), expired: false }]);
    assert.equal(await started, true); await settle();
    assert.deepEqual(f.events, [["record", 9], ["catchup"]]);
    assert.equal(f.mailbox.tick(1050), true); await settle();
    assert.equal(f.streams[0].closed, true); assert.equal(f.mailbox.healthy(), false);
    assert.equal(f.mailbox.recovering(), true);
    assert.deepEqual(f.events.at(-1), ["status", "reconnecting"]);
    f.lookups.at(-1).resolve([]); await settle();
    assert.equal(f.mailbox.healthy(), true);
    assert.deepEqual(f.events.slice(-2), [["status", "ready"], ["catchup"]]);
    f.mailbox.close(); assert.ok(f.streams.every((stream) => stream.closed));
});

test("long jumps retire pending windows and closing suppresses late completion", async () => {
    const f = fixture(), started = f.mailbox.start(1000); await settle();
    assert.equal(f.mailbox.tick(2000), true); await settle();
    assert.ok(f.streams.slice(0, 4).every((stream) => stream.closed));
    for (const item of f.lookups.slice(0, 4)) item.resolve([{ bytes: Uint8Array.of(1), expired: false }]);
    await settle(); assert.deepEqual(f.events, []);
    f.mailbox.close(); assert.equal(await started, false);
    for (const item of f.lookups.slice(4)) item.resolve([]);
    await settle(); assert.deepEqual(f.events, []);
});

test("one failed window recovers before the combined watch becomes ready", async () => {
    const f = fixture({ retryDelayMs: 50 }); await ready(f);
    f.streams[2].done.reject(new ProxyError("stream_closed", "offline")); await settle();
    assert.equal(f.mailbox.recovering(), true);
    assert.deepEqual(f.events.at(-1), ["status", "reconnecting"]);
    f.fire(); await settle();
    assert.equal(f.lookups.at(-1).window, 7);
    f.lookups.at(-1).resolve([]); await settle();
    assert.equal(f.mailbox.healthy(), true);
    assert.deepEqual(f.events.slice(-2), [["status", "ready"], ["catchup"]]);
    f.mailbox.close();
});

test("invalid clocks and failed initial lookups close all windows", async () => {
    const f = fixture(), started = f.mailbox.start(1000); await settle();
    f.lookups[1].reject(new ProxyError("network_error", "offline"));
    assert.equal(await started, false); await settle();
    assert.equal(f.mailbox.healthy(), false); assert.ok(f.streams.every((stream) => stream.closed));
    assert.equal(f.events.filter((event) => event[0] === "error").length, 1);
    const g = fixture(); await ready(g);
    assert.equal(g.mailbox.tick(999), false);
    assert.equal(g.events.at(-1)[0], "error"); g.mailbox.close();
});
