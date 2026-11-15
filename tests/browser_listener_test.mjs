import test from "node:test";
import assert from "node:assert/strict";
import { BrowserListener } from "../src/browser/browser_listener.mjs";

function deferred() {
    let resolve;
    const promise = new Promise((done) => { resolve = done; });
    return { promise, resolve };
}
function fixture() {
    const events = [], timers = new Map(), publication = deferred(), done = deferred();
    let id = 0, listener, known = false, current = true, watchHealthy = true, recovering = false, nowMs = 1000000;
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 100, _free: () => {},
        _p2pdc_discovery_create: () => 1, _p2pdc_discovery_destroy: () => {}, _p2pdc_discovery_healthy: () => 1,
        _p2pdc_discovery_key: (_id, ptr, size) => { module.HEAPU8.fill(1, ptr, ptr + size); return 1; },
        _p2pdc_discovery_self: (_id, ptr, size) => { module.HEAPU8.fill(2, ptr, ptr + size); return 1; },
        _p2pdc_discovery_publish: (_id, _time, ptr, size) => { module.HEAPU8.fill(3, ptr, ptr + size); return 1; },
        _p2pdc_discovery_receive: () => 1, _p2pdc_discovery_sweep: () => 1, _p2pdc_discovery_current: () => 1,
        _p2pdc_discovery_incoming: (_local, _watch, ptr, _size, _time, inspect, out) => {
            if (module.HEAPU8[ptr] === 2 || module.HEAPU8[ptr] === 3) return 4;
            if (module.HEAPU8[ptr] !== 1) return 0;
            if (!known) return 3;
            if (inspect) return 5;
            module.HEAPU8.fill(0, out, out + 20); module.HEAPU8[out + 3] = 7;
            module.HEAPU8.fill(9, out + 4, out + 20); return 1;
        } };
    const peer = { handle: 7, signingKey: new Uint8Array(32).fill(4), recipientKey: new Uint8Array(32).fill(5),
        instance: new Uint8Array(16).fill(6), inbox: new Uint8Array(20).fill(7),
        sequence: 1n, features: 0n, issuedAt: 1000n, expiresAt: 1120n };
    const watch = { windowed: false, discovery: { module, id: 2 }, healthy: () => watchHealthy, recovering: () => recovering,
        current: (handle) => current && handle === 7 ? peer : null };
    const proxy = { listen: (_key, callback) => { listener = callback; return {
        ready: Promise.resolve(true), done: done.promise, close: () => done.resolve() }; },
        get: async () => [], put: () => publication.promise };
    const identity = { identitySeed: new Uint8Array(32).fill(1), recipientSecret: new Uint8Array(32).fill(2),
        capability: new Uint8Array(32).fill(3) };
    const handlers = { onPublished: () => events.push(["published"]), onRequest: (v) => events.push(["request", v]),
        onStatus: (v) => events.push(["status", v]), onError: (v) => events.push(["error", v]) };
    const options = { now: () => 1000, windowed: false, deadlineNow: () => nowMs, timers: { set: (callback, delay) => {
        const handle = ++id; timers.set(handle, { callback, delay }); return handle;
    }, clear: (handle) => timers.delete(handle) } };
    return { module, watch, proxy, identity, handlers, options, events, publication, timers, peer,
        known: (value) => { known = value; }, current: (value) => { current = value; },
        healthy: (value) => { watchHealthy = value; }, setNowMs: (value) => { nowMs = value; },
        recovering: (value) => { recovering = value; watchHealthy = !value; },
        emit: (values) => listener({ bytes: Uint8Array.from(values), expired: false }),
        fire: (delay) => { const [key, timer] = [...timers].find(([, value]) => value.delay === delay);
            timers.delete(key); timer.callback(); } };
}
async function settle() { for (let i = 0; i < 15; ++i) await Promise.resolve(); }
async function started(f) {
    const listener = new BrowserListener(f.module, f.proxy, f.identity, f.watch, f.handlers, f.options);
    assert.equal(await listener.start(), true); await settle(); return listener;
}

test("listener rejects mismatched discovery protocols", () => {
    const f = fixture(); f.watch.windowed = true;
    assert.throws(() => new BrowserListener(f.module, f.proxy, f.identity, f.watch, f.handlers, f.options),
        /protocols must match/);
    assert.equal(f.timers.size, 0);
});

test("listener retains early records and asks for consent after discovery", async () => {
    const f = fixture(), listener = await started(f), delivered = [];
    try {
        f.emit([2]); f.emit([1]); await settle();
        assert.equal(listener.state, "starting");
        f.publication.resolve(); await settle();
        assert.equal(listener.state, "listening");
        f.known(true); assert.equal(listener.retry(), true); await settle();
        assert.equal(listener.state, "pending");
        assert.equal(f.events.filter((event) => event[0] === "request").length, 1);
        f.events.find((event) => event[0] === "request")[1].signingKey.fill(0);
        f.emit([3]); await settle();
        const offer = listener.accept((record) => delivered.push([...record]));
        assert.equal(offer.peer.signingKey[0], 4); assert.deepEqual([...offer.offer], [1]);
        await settle(); assert.deepEqual(delivered, []);
        assert.equal(listener.resume(), true); assert.equal(listener.resume(), false); await settle();
        assert.deepEqual(delivered, [[2], [3]]);
        assert.equal(listener.accept(() => {}), null);
    } finally { listener.close(); }
    assert.equal(f.timers.size, 0);
});

test("stale or expired requests fail and pending timeout closes resources", async () => {
    const f = fixture(), listener = await started(f);
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    f.current(false); assert.equal(listener.accept(() => {}), null); await settle();
    assert.equal(listener.state, "failed"); assert.deepEqual(f.events.at(-1), ["error", "stale"]);
    listener.close();
    const g = fixture(), other = await started(g);
    g.publication.resolve(); g.known(true); await settle(); g.emit([1]); await settle();
    g.setNowMs(1030000); g.fire(30000); await settle();
    assert.equal(other.state, "failed"); assert.deepEqual(g.events.at(-1), ["error", "timeout"]);
    assert.equal(other.accept(() => {}), null); other.close();
    const h = fixture(), delayed = await started(h);
    h.publication.resolve(); h.known(true); await settle(); h.emit([1]); await settle();
    h.setNowMs(1030000); assert.equal(delayed.accept(() => {}), null); await settle();
    assert.deepEqual(h.events.at(-1), ["error", "timeout"]); delayed.close();
});

test("pending consent survives recovery and filters buffered records before acceptance", async () => {
    const f = fixture(), listener = await started(f), delivered = [];
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    assert.equal(listener.state, "pending");
    f.recovering(true); f.emit([2]); f.emit([9]); f.emit([1, 9]); f.emit([3]); await settle();
    assert.equal(listener.accept(() => {}), null); assert.equal(listener.state, "pending");
    f.recovering(false); assert.equal(listener.retry(), true);
    assert.ok(listener.accept((wire) => delivered.push([...wire])));
    assert.equal(listener.resume(), true); await settle();
    assert.deepEqual(delivered, [[2], [3]]); listener.close();
});

test("pending consent still expires or fails stale after recovery", async () => {
    const f = fixture(), listener = await started(f);
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    f.recovering(true); f.setNowMs(1030000);
    assert.equal(listener.accept(() => {}), null); await settle();
    assert.deepEqual(f.events.at(-1), ["error", "timeout"]); listener.close();
    const g = fixture(), other = await started(g);
    g.publication.resolve(); g.known(true); await settle(); g.emit([1]); await settle();
    g.recovering(true); g.emit([2]); await settle();
    g.current(false); g.recovering(false); assert.equal(other.accept(() => {}), null);
    await settle(); assert.deepEqual(g.events.at(-1), ["error", "stale"]); other.close();
});

test("pending recovery bounds unverified signaling", async () => {
    const f = fixture(), listener = await started(f);
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    f.recovering(true);
    for (let index = 0; index < 65; ++index) {
        f.emit([9, index]);
        if (index % 20 === 19) await settle();
    }
    await settle();
    assert.equal(listener.state, "failed");
    assert.deepEqual(f.events.at(-1), ["error", "backpressure"]); listener.close();
});

test("listener bounds backlog and close suppresses pending events", async () => {
    const f = fixture(), listener = await started(f);
    for (let index = 0; index < 65; ++index) f.emit([2, index]);
    await settle(); assert.equal(listener.state, "failed");
    assert.deepEqual(f.events.at(-1), ["error", "backpressure"]); listener.close();
    const g = fixture(), other = await started(g);
    g.publication.resolve(); g.known(true); await settle(); g.emit([1]);
    other.close(); await settle();
    assert.equal(g.events.some((event) => event[0] === "request"), false);
    assert.equal(g.timers.size, 0);
});

test("duplicate proxy records do not exhaust the backlog", async () => {
    const f = fixture(), listener = await started(f);
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    for (let index = 0; index < 100; ++index) {
        f.emit([1]); f.emit([2]);
        if (index % 25 === 24) await settle();
    }
    await settle();
    assert.equal(listener.state, "pending");
    const records = [];
    assert.ok(listener.accept((wire) => records.push([...wire])));
    assert.equal(listener.resume(), true); await settle();
    assert.deepEqual(records, [[2]]); listener.close();
});

test("invalid records are discarded while authenticated early records are retained", async () => {
    const f = fixture(), listener = await started(f), delivered = [];
    f.publication.resolve(); await settle();
    for (let index = 0; index < 100; ++index) {
        f.emit([9, index]);
        if (index % 25 === 24) await settle();
    }
    assert.equal(listener.state, "listening");
    f.emit([2]); f.known(true); f.emit([1]); await settle();
    assert.equal(listener.state, "pending");
    for (let index = 0; index < 100; ++index) {
        f.emit([9, index]);
        if (index % 25 === 24) await settle();
    }
    assert.equal(listener.state, "pending");
    assert.ok(listener.accept((wire) => delivered.push([...wire])));
    assert.equal(listener.resume(), true); await settle();
    assert.deepEqual(delivered, [[2]]); listener.close();
});

test("closed and unhealthy watches cannot be accepted", async () => {
    const f = fixture(), listener = await started(f);
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    f.emit([2]); await settle();
    f.healthy(false); assert.equal(listener.accept(() => {}), null);
    await settle(); assert.deepEqual(f.events.at(-1), ["error", "stale"]); listener.close();
    const g = fixture(), other = await started(g);
    g.publication.resolve(); g.known(true); await settle(); g.emit([1]); await settle();
    g.watch.discovery.id = 3; assert.equal(other.accept(() => {}), null);
    await settle(); assert.deepEqual(g.events.at(-1), ["error", "stale"]); other.close();
});

test("early timer is rescheduled and clock regression rejects consent", async () => {
    const f = fixture(), listener = await started(f);
    f.publication.resolve(); f.known(true); await settle(); f.emit([1]); await settle();
    f.setNowMs(1010000); f.fire(30000); assert.equal(listener.state, "pending");
    assert.ok([...f.timers.values()].some((timer) => timer.delay === 20000));
    f.setNowMs(1009999); assert.equal(listener.accept(() => {}), null);
    await settle(); assert.deepEqual(f.events.at(-1), ["error", "clock"]); listener.close();
});
