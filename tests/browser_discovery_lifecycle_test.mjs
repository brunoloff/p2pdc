import test from "node:test";
import assert from "node:assert/strict";
import { BrowserLease } from "../src/browser/browser_lease.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
function fixture() {
    let now = 1000, nextTimer = 1, record, subscriptionClosed = false, publishCount = 0;
    const timers = new Map(), writes = [], events = [], errors = [];
    const module = { HEAPU8: new Uint8Array(4096), _malloc: () => 100, _free: () => {},
        _p2pdc_discovery_create: () => 1, _p2pdc_discovery_destroy: () => {}, _p2pdc_discovery_healthy: () => 1,
        _p2pdc_discovery_key: (_id, pointer, size) => { module.HEAPU8.fill(1, pointer, pointer + size); return 1; },
        _p2pdc_discovery_window_key: (_id, value, pointer, size) => { module.HEAPU8.fill(Math.floor(value / 120), pointer, pointer + size); return 1; },
        _p2pdc_discovery_self: (_id, pointer, size) => { module.HEAPU8.fill(2, pointer, pointer + size); return 1; },
        _p2pdc_discovery_publish: (_id, _now, pointer, size) => { module.HEAPU8.fill(++publishCount, pointer, pointer + size); return 1; },
        _p2pdc_discovery_publish_windowed: (_id, _now, pointer, size) => { module.HEAPU8.fill(++publishCount, pointer, pointer + size); return 1; },
        _p2pdc_discovery_receive: () => { module.p2pdcDiscoveryBridge.event(1, 1, 0, new Uint8Array(132)); return 0; },
        _p2pdc_discovery_sweep: () => 1, _p2pdc_discovery_current: () => 0 };
    const options = { identitySeed: new Uint8Array(32).fill(1), recipientSecret: new Uint8Array(32).fill(2),
        capability: new Uint8Array(32).fill(3) };
    const opened = deferred(), done = deferred(), lookup = deferred();
    const proxy = {
        listen: (_key, callback) => { record = callback; return { ready: opened.promise, done: done.promise,
            close: () => { subscriptionClosed = true; done.resolve(); } }; },
        get: () => lookup.promise,
        put: (key, bytes) => { const item = deferred(); writes.push({ key: [...key], data: [...bytes], ...item }); return item.promise; },
    };
    const clockOptions = { now: () => now, windowed: false, timers: {
        set: (callback, delay) => { const id = nextTimer++; timers.set(id, { callback, delay }); return id; },
        clear: (id) => timers.delete(id),
    } };
    return { module, options, proxy, clockOptions, timers, writes, events, errors, opened, done, lookup,
        record: (bytes) => record({ bytes: Uint8Array.from(bytes), expired: false }),
        setNow: (value) => { now = value; },
        fire: () => { const [id, timer] = timers.entries().next().value; timers.delete(id); timer.callback(); },
        get subscriptionClosed() { return subscriptionClosed; } };
}
async function settle() { for (let i = 0; i < 10; ++i) await Promise.resolve(); }

test("watch receives verified events, sweeps and suppresses late callbacks", async () => {
    const f = fixture();
    const watch = new BrowserWatch(f.module, f.proxy, f.options, { onPeer: (peer, event) => f.events.push([peer.handle, event]),
        onCatchup: () => f.events.push("catchup"), onError: (error) => f.errors.push(error) }, f.clockOptions);
    const started = watch.start(); f.opened.resolve(true); f.lookup.resolve([]);
    assert.equal(await started, true); f.record(new Uint8Array(242)); await settle();
    assert.deepEqual(f.events, ["catchup", [1, "seen"]]);
    assert.equal(watch.healthy(), true); assert.equal(watch.current(1), null);
    f.setNow(1001); f.fire(); assert.equal(f.timers.size, 1);
    f.record(new Uint8Array(242)); watch.close(); await settle();
    assert.equal(f.events.length, 2); assert.equal(f.subscriptionClosed, true);
    assert.equal(f.timers.size, 0); assert.equal(watch.healthy(), false);
});

test("watch absorbs a small clock regression, terminates on a large one and reports once", async () => {
    const f = fixture();
    const watch = new BrowserWatch(f.module, f.proxy, f.options, { onPeer: () => {}, onCatchup: () => {},
        onError: (error) => f.errors.push(error) }, f.clockOptions);
    const started = watch.start(); f.opened.resolve(true); f.lookup.resolve([]); await started;
    f.record([1]); await settle();
    f.setNow(999); f.fire(); await settle();
    assert.deepEqual(f.errors, []); assert.equal(watch.healthy(), true);
    f.setNow(969); f.fire(); await settle();
    assert.deepEqual(f.errors, ["clock"]); assert.equal(watch.healthy(), false);
    watch.close();
});

test("windowed watch starts its shared tracker and rejects invalid mode", async () => {
    const f = fixture();
    const handlers = { onPeer: (peer, event) => f.events.push([peer.handle, event]),
        onCatchup: () => f.events.push("catchup"), onError: (error) => f.errors.push(error) };
    assert.throws(() => new BrowserWatch(f.module, f.proxy, f.options, handlers,
        { ...f.clockOptions, windowed: "yes" }), TypeError);
    const watch = new BrowserWatch(f.module, f.proxy, f.options, handlers,
        { ...f.clockOptions, windowed: true });
    const started = watch.start(); f.opened.resolve(true); f.lookup.resolve([]);
    assert.equal(await started, true); f.record(new Uint8Array(242)); await settle();
    assert.deepEqual(f.events, ["catchup", [1, "seen"]]);
    assert.equal(watch.healthy(), true); assert.deepEqual(f.errors, []);
    watch.close();
});

test("standalone browser discovery defaults to rotating keys", async () => {
    const f = fixture(), options = { ...f.clockOptions }; delete options.windowed;
    const watch = new BrowserWatch(f.module, f.proxy, f.options, { onPeer: () => {}, onCatchup: () => {},
        onError: (error) => f.errors.push(error) }, options);
    assert.equal(watch.windowed, true); watch.close();
    const g = fixture(), leaseOptions = { ...g.clockOptions }; delete leaseOptions.windowed;
    const lease = new BrowserLease(g.module, g.proxy, g.options, { onPublished: () => {},
        onError: (error) => g.errors.push(error) }, leaseOptions);
    assert.equal(lease.windowed, true); assert.equal(lease.start(), true); await settle();
    assert.equal(g.writes.length, 1); assert.deepEqual(g.errors, []); lease.close();
});

test("lease refreshes, retries retained bytes and counts attempts", async () => {
    const f = fixture();
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => f.events.push("published"),
        onError: (error) => f.errors.push(error) }, f.clockOptions);
    assert.equal(lease.start(), true); assert.equal(lease.self(), null); await settle();
    assert.equal(f.writes.length, 1); f.writes[0].resolve(); await settle();
    assert.equal(f.events.length, 1); assert.ok(lease.self());
    f.setNow(1060); f.fire(); await settle();
    assert.equal(f.writes.length, 2); f.writes[1].reject(new Error("offline")); await settle();
    assert.deepEqual(f.errors, ["retrying"]);
    f.setNow(1062); f.fire(); await settle();
    assert.deepEqual(f.writes[2].data, f.writes[1].data);
    f.writes[2].resolve(); await settle();
    assert.equal(f.events.length, 2); lease.close();
});

test("windowed lease retains retry key and rotates after completion", async () => {
    const f = fixture();
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => f.events.push("published"),
        onError: (error) => f.errors.push(error) }, { ...f.clockOptions, windowed: true, maxPublications: 3 });
    assert.equal(lease.start(), true); await settle(); f.writes[0].resolve(); await settle();
    f.setNow(1060); f.fire(); await settle();
    f.writes[1].reject(new Error("offline")); await settle();
    f.setNow(1081); f.fire(); await settle();
    assert.deepEqual(f.writes[2].key, f.writes[1].key);
    assert.deepEqual(f.writes[2].data, f.writes[1].data);
    f.writes[2].resolve(); await settle();
    f.setNow(1120); f.fire(); await settle();
    assert.notDeepEqual(f.writes[3].key, f.writes[2].key);
    f.writes[3].resolve(); await settle();
    assert.equal(f.events.length, 3); assert.deepEqual(f.errors, ["retrying"]); lease.close();
});

test("windowed lease enforces a per-window publication limit", async () => {
    const f = fixture();
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => {},
        onError: (error) => f.errors.push(error) }, { ...f.clockOptions, windowed: true, maxPublications: 1 });
    assert.equal(lease.start(), true); await settle(); f.writes[0].resolve(); await settle();
    f.setNow(1060); f.fire(); await settle();
    assert.deepEqual(f.errors, ["publicationLimit"]); lease.close();
    const g = fixture(); g.setNow(1070);
    const next = new BrowserLease(g.module, g.proxy, g.options, { onPublished: () => {},
        onError: (error) => g.errors.push(error) }, { ...g.clockOptions, windowed: true, maxPublications: 1 });
    assert.equal(next.start(), true); await settle(); g.writes[0].resolve(); await settle();
    g.setNow(1130); g.fire(); await settle();
    assert.equal(g.writes.length, 2); assert.notDeepEqual(g.writes[0].key, g.writes[1].key);
    assert.deepEqual(g.errors, []); next.close();
});

test("windowed lease reports a missing key bridge and rejects invalid mode", async () => {
    const f = fixture();
    const handlers = { onPublished: () => {}, onError: (error) => f.errors.push(error) };
    assert.throws(() => new BrowserLease(f.module, f.proxy, f.options, handlers,
        { ...f.clockOptions, windowed: "yes" }), TypeError);
    delete f.module._p2pdc_discovery_window_key;
    const lease = new BrowserLease(f.module, f.proxy, f.options, handlers,
        { ...f.clockOptions, windowed: true });
    assert.equal(lease.start(), false); await settle();
    assert.deepEqual(f.errors, ["discovery"]); assert.equal(f.writes.length, 0); lease.close();
});

test("lease expires pending publication and honors publication limit", async () => {
    const f = fixture();
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => {},
        onError: (error) => f.errors.push(error) }, f.clockOptions);
    assert.equal(lease.start(), true); await settle();
    f.setNow(1120); f.fire(); await settle();
    assert.deepEqual(f.errors, ["expiredPending"]);
    f.writes[0].resolve(); await settle();
    assert.equal(lease.healthy(), false); lease.close();
    const g = fixture();
    const limited = new BrowserLease(g.module, g.proxy, g.options, { onPublished: () => {},
        onError: (error) => g.errors.push(error) }, { ...g.clockOptions, maxPublications: 1 });
    assert.equal(limited.start(), true); await settle(); g.writes[0].resolve(); await settle();
    g.setNow(1060); g.fire(); await settle();
    assert.deepEqual(g.errors, ["publicationLimit"]); limited.close();
});

test("lease absorbs small clock regressions, fails on large ones, and close suppresses late callbacks", async () => {
    const f = fixture();
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => f.events.push("published"),
        onError: (error) => f.errors.push(error) }, f.clockOptions);
    assert.equal(lease.start(), true); await settle(); f.writes[0].resolve(); await settle();
    f.setNow(999); assert.notEqual(lease.self(), null); await settle();
    assert.deepEqual(f.errors, []);
    f.setNow(969); assert.equal(lease.self(), null); await settle();
    assert.deepEqual(f.errors, ["clock"]); lease.close();
    const g = fixture();
    const other = new BrowserLease(g.module, g.proxy, g.options, { onPublished: () => g.events.push("published"),
        onError: (error) => g.errors.push(error) }, g.clockOptions);
    assert.equal(other.start(), true); await settle(); other.close(); g.writes[0].resolve(); await settle();
    assert.deepEqual(g.events, []); assert.deepEqual(g.errors, []);
});

test("a stalled retry cannot outlive the pending advertisement", async () => {
    const f = fixture();
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => {},
        onError: (error) => f.errors.push(error) }, f.clockOptions);
    assert.ok(lease.endpoint());
    assert.equal(lease.start(), true); await settle();
    f.writes[0].reject(new Error("offline")); await settle();
    f.setNow(1002); f.fire(); await settle();
    assert.equal(f.writes.length, 2);
    f.setNow(1120); f.fire(); await settle();
    assert.deepEqual(f.errors, ["retrying", "expiredPending"]);
    assert.equal(lease.healthy(), false); lease.close();
});

test("watch sweeps while initial lookup remains pending and rejects bad handles", async () => {
    const f = fixture();
    let sweeps = 0;
    f.module._p2pdc_discovery_sweep = () => { ++sweeps; return 1; };
    const watch = new BrowserWatch(f.module, f.proxy, f.options, { onPeer: () => {}, onCatchup: () => {},
        onError: (error) => f.errors.push(error) }, f.clockOptions);
    const pending = watch.start();
    f.opened.resolve(true); await settle();
    f.record(new Uint8Array(242)); await settle();
    f.setNow(1001); f.fire();
    assert.equal(sweeps, 1);
    assert.throws(() => watch.current(0), TypeError);
    assert.equal(watch.healthy(), true);
    f.lookup.resolve([]); assert.equal(await pending, true);
    watch.close();
});

test("failed timer scheduling makes both starts fail", async () => {
    const f = fixture();
    const broken = { ...f.clockOptions, timers: { set: () => { throw new Error("timer failed"); }, clear: () => {} } };
    const watch = new BrowserWatch(f.module, f.proxy, f.options, { onPeer: () => {}, onCatchup: () => {},
        onError: (error) => f.errors.push(error) }, broken);
    assert.equal(await watch.start(), false); await settle();
    assert.deepEqual(f.errors, ["scheduler"]); watch.close();
    const g = fixture();
    const lease = new BrowserLease(g.module, g.proxy, g.options, { onPublished: () => {},
        onError: (error) => g.errors.push(error) }, { ...g.clockOptions, timers: broken.timers });
    assert.equal(lease.start(), false); await settle();
    assert.deepEqual(g.errors, ["scheduler"]); assert.equal(g.writes.length, 0); lease.close();
    const h = fixture();
    const noHandle = { set: () => null, clear: () => {} };
    const other = new BrowserWatch(h.module, h.proxy, h.options, { onPeer: () => {}, onCatchup: () => {},
        onError: (error) => h.errors.push(error) }, { ...h.clockOptions, timers: noHandle });
    assert.equal(await other.start(), false); await settle();
    assert.deepEqual(h.errors, ["scheduler"]); other.close();
});

test("a timer that cannot be canceled cannot revive an old lease deadline", async () => {
    const f = fixture();
    const timers = { set: f.clockOptions.timers.set, clear: () => { throw new Error("cannot cancel"); } };
    const lease = new BrowserLease(f.module, f.proxy, f.options, { onPublished: () => {},
        onError: (error) => f.errors.push(error) }, { ...f.clockOptions, timers });
    assert.equal(lease.start(), true); await settle();
    f.writes[0].resolve(); await settle();
    f.setNow(1120); f.fire(); await settle();
    assert.equal(lease.healthy(), true);
    assert.deepEqual(f.errors, []);
    lease.close();
});

test("callbacks may close their watch or lease without later notifications", async () => {
    const f = fixture();
    let watch;
    watch = new BrowserWatch(f.module, f.proxy, f.options, { onPeer: () => { f.events.push("peer"); watch.close(); },
        onCatchup: () => {}, onError: (error) => f.errors.push(error) }, f.clockOptions);
    const pending = watch.start(); f.opened.resolve(true); f.lookup.resolve([]); await pending;
    f.record(new Uint8Array(242)); await settle();
    assert.deepEqual(f.events, ["peer"]); assert.equal(f.timers.size, 0);
    const g = fixture();
    let lease;
    lease = new BrowserLease(g.module, g.proxy, g.options, { onPublished: () => { g.events.push("published"); lease.close(); },
        onError: (error) => g.errors.push(error) }, g.clockOptions);
    assert.equal(lease.start(), true); await settle(); g.writes[0].resolve(); await settle();
    assert.deepEqual(g.events, ["published"]); assert.equal(g.timers.size, 0);
});
