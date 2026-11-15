import test from "node:test";
import assert from "node:assert/strict";
import { BrowserIncoming } from "../src/browser/browser_incoming.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

function deferred() {
    let resolve;
    const promise = new Promise((done) => { resolve = done; });
    return { promise, resolve };
}
class FakePeer {
    constructor() { this.connectionState = "new"; this.iceConnectionState = "new"; this.sctp = { maxMessageSize: 65536 }; }
    async getStats() { return new Map(); }
    close() { this.connectionState = "closed"; }
    connect() {
        const channel = { label: "p2pdc/reliable/v1", ordered: true, maxPacketLifeTime: null, maxRetransmits: null,
            readyState: "open", bufferedAmount: 0, close() { this.readyState = "closed"; }, send() {} };
        this.ondatachannel({ channel }); this.connectionState = "connected";
        this.iceConnectionState = "connected"; this.onconnectionstatechange();
    }
}
function fixture() {
    let current = true, callback, transport, captured, destroyed = 0, timerId = 0, signalFailure = false;
    const events = [], records = [], timers = new Map(), opened = deferred(), done = deferred(), published = deferred();
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 100, _free: () => {},
        _p2pdc_discovery_create: () => 1, _p2pdc_discovery_destroy: () => {}, _p2pdc_discovery_healthy: () => 1,
        _p2pdc_discovery_key: (_id, ptr, size) => { module.HEAPU8.fill(1, ptr, ptr + size); return 1; },
        _p2pdc_discovery_self: (_id, ptr, size) => { module.HEAPU8.fill(2, ptr, ptr + size); return 1; },
        _p2pdc_discovery_publish: (_id, _time, ptr, size) => { module.HEAPU8.fill(3, ptr, ptr + size); return 1; },
        _p2pdc_discovery_receive: () => 1, _p2pdc_discovery_sweep: () => 1, _p2pdc_discovery_current: () => 1,
        _p2pdc_discovery_incoming: (_local, _watch, ptr, _size, _time, inspect, out) => {
            if (module.HEAPU8[ptr] !== 1) return 4;
            if (inspect) return 5;
            module.HEAPU8.fill(0, out, out + 20); module.HEAPU8[out + 3] = 7;
            module.HEAPU8.fill(9, out + 4, out + 20); return 1;
        },
        _p2pdc_secure_create: (ptr, size) => { captured = module.HEAPU8.slice(ptr, ptr + size); return 1; },
        _p2pdc_secure_destroy: () => { ++destroyed; }, _p2pdc_secure_description: () => 0,
        _p2pdc_secure_candidate: () => 0, _p2pdc_secure_receive: (_id, ptr, size) => {
            records.push([...module.HEAPU8.slice(ptr, ptr + size)]); return 0;
        }, _p2pdc_secure_description_done: () => 0, _p2pdc_secure_candidate_done: () => 0,
        _p2pdc_secure_ready: () => 0, _p2pdc_secure_frame: () => 0, _p2pdc_secure_send: () => 0,
        _p2pdc_secure_wake: () => 0, _p2pdc_secure_timeout: () => 0 };
    const peer = { handle: 7, signingKey: new Uint8Array(32).fill(4), recipientKey: new Uint8Array(32).fill(5),
        instance: new Uint8Array(16).fill(6), inbox: new Uint8Array(20).fill(7),
        sequence: 1n, features: 0n, issuedAt: 1000n, expiresAt: 1120n };
    const watch = { windowed: false, healthy: () => true, current: () => current ? peer : null,
        discovery: { module, id: 2, key: () => new Uint8Array(20).fill(1), self: () => ({
            signingKey: new Uint8Array(32).fill(2), recipientKey: new Uint8Array(32).fill(2) }) } };
    const proxy = { listen: (_key, fn) => { callback = fn; return { ready: opened.promise,
        done: done.promise, close: () => done.resolve() }; }, get: async () => [],
    put: () => signalFailure ? Promise.reject(new Error("offline")) : published.promise };
    const identity = { identitySeed: new Uint8Array(32).fill(8), recipientSecret: new Uint8Array(32).fill(9),
        capability: new Uint8Array(32).fill(10) };
    const options = { now: () => 1000, transport: { relayPolicy: "automatic",
        peerConnectionFactory: () => { transport = new FakePeer(); return transport; } },
    listener: { now: () => 1000, windowed: false, deadlineNow: () => 1000000, timers: {
        set: (fn, delay) => { const id = ++timerId; timers.set(id, { fn, delay }); return id; },
        clear: (id) => timers.delete(id) } } };
    const handlers = { onPublished: () => events.push(["published"]), onRequest: (v) => events.push(["request", v]),
        onState: (v) => events.push(["state", v]), onRoute: (v) => events.push(["route", v]),
        onMessage: (v) => events.push(["message", v]), onWritable: () => events.push(["writable"]),
        onStatus: (v) => events.push(["status", v]), onError: (v) => events.push(["error", v]) };
    return { module, watch, peer, proxy, identity, options, handlers, events, records, opened, published, timers,
        emit: (value) => callback({ bytes: Uint8Array.from(value), expired: false }), stale: () => { current = false; },
        failSignal: () => { signalFailure = true; }, transport: () => transport, captured: () => captured,
        destroyed: () => destroyed };
}
async function settle() { for (let i = 0; i < 25; ++i) await Promise.resolve(); }
async function readyListener(f) {
    const owner = new BrowserIncoming(f.module, f.proxy, f.identity, f.watch, f.options, f.handlers);
    const started = owner.start(); f.opened.resolve(true); assert.equal(await started, true);
    f.published.resolve(); await settle(); return owner;
}

test("incoming rejects invalid application limits before starting a listener", () => {
    const f = fixture(); f.options.maxMessageBytes = 0;
    assert.throws(() => new BrowserIncoming(f.module, f.proxy, f.identity, f.watch, f.options, f.handlers), RangeError);
    assert.equal(f.timers.size, 0); assert.equal(f.transport(), undefined);
});

test("incoming owner waits for consent and hands the offer off before queued records", async () => {
    const f = fixture(); f.options.maxMessageBytes = 8192;
    const owner = await readyListener(f);
    try {
        f.emit([1]); f.emit([2]); await settle();
        assert.equal(owner.state, "pending"); assert.equal(owner.send(Uint8Array.of(1)), "notReady");
        assert.equal(owner.maxMessageSize(), null);
        f.identity.identitySeed.fill(0); f.identity.recipientSecret.fill(0); f.identity.capability.fill(0);
        assert.equal(await owner.accept(), true); await settle();
        assert.deepEqual(f.records, [[1], [2]]); assert.equal(f.captured()[0], 8);
        assert.equal(f.captured()[32], 9); assert.equal(f.captured()[64], 10); assert.equal(owner.identity, null);
        assert.equal(new DataView(f.captured().buffer).getUint32(209), 8192);
        f.transport().connect(); await settle(); f.module.p2pdcSecureBridge.number(1, 0, 8192); await settle();
        assert.equal(owner.state, "ready"); assert.equal(owner.send(Uint8Array.of(1)), "accepted");
        assert.equal(owner.maxMessageSize(), 8192);
        assert.equal(owner.listener.lease.closed, true); assert.equal(f.timers.size, 0);
    } finally { owner.close(); }
    assert.equal(f.transport().connectionState, "closed"); assert.equal(f.destroyed(), 1);
    assert.equal(owner.maxMessageSize(), null);
});

test("stale peer and explicit local rejection never create a connection", async () => {
    const f = fixture(), owner = await readyListener(f);
    f.emit([1]); await settle(); f.stale();
    assert.equal(await owner.accept(), false); await settle();
    assert.equal(owner.state, "failed"); assert.equal(f.transport(), undefined);
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "stale"]]); owner.close();
    const g = fixture(), other = await readyListener(g);
    g.emit([1]); await settle(); assert.equal(other.reject(), true); assert.equal(other.reject(), false);
    assert.equal(other.state, "closed"); assert.equal(g.transport(), undefined); assert.equal(g.timers.size, 0);
});

test("closing during startup suppresses publication and invalid setup fails immediately", async () => {
    const f = fixture();
    f.watch.discovery.key = () => new Uint8Array(20).fill(9);
    assert.throws(() => new BrowserIncoming(f.module, f.proxy, f.identity, f.watch, f.options, f.handlers));
    const g = fixture(), owner = new BrowserIncoming(g.module, g.proxy, g.identity, g.watch, g.options, g.handlers);
    const started = owner.start(); owner.close(); g.opened.resolve(true);
    assert.equal(await started, false); await settle();
    assert.deepEqual(g.events, []); assert.equal(g.timers.size, 0);
});

test("close during acceptance suppresses signaling and callback delivery", async () => {
    const f = fixture(), owner = await readyListener(f);
    f.emit([1]); await settle();
    const accepted = owner.accept(); owner.close();
    assert.equal(await accepted, false); await settle();
    assert.equal(owner.state, "closed"); assert.deepEqual(f.records, []);
    assert.equal(f.transport().connectionState, "closed"); assert.equal(f.timers.size, 0);
});

test("closing from the connecting callback cancels acceptance", async () => {
    const f = fixture();
    let owner;
    f.handlers.onState = (value) => { if (value === "connecting") owner.close(); };
    owner = await readyListener(f);
    f.emit([1]); await settle();
    assert.equal(await owner.accept(), false); await settle();
    assert.equal(owner.state, "closed"); assert.deepEqual(f.records, []);
    assert.equal(f.transport().connectionState, "closed"); assert.equal(f.timers.size, 0);
});

test("incoming signaling publication failure closes the connection", async () => {
    const f = fixture(), owner = await readyListener(f);
    f.emit([1]); await settle(); assert.equal(await owner.accept(), true);
    f.failSignal();
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 0, Uint8Array.of(77)), true);
    await settle();
    assert.equal(owner.state, "failed");
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "signaling"]]);
    assert.equal(f.transport().connectionState, "closed"); owner.close();
});

test("incoming retains an authenticated channel when signaling disconnects", async () => {
    const f = fixture(), owner = await readyListener(f);
    try {
        f.emit([1]); await settle(); assert.equal(await owner.accept(), true);
        f.transport().connect(); await settle(); f.module.p2pdcSecureBridge.number(1, 0, 4096); await settle();
        owner.listener.mailbox.fail(new ProxyError("stream_closed", "offline")); await settle();
        assert.equal(owner.state, "ready"); assert.equal(owner.send(Uint8Array.of(3)), "accepted");
        assert.deepEqual(f.events.filter((e) => e[0] === "status"), [["status", "signalingUnavailable"]]);
        f.module.p2pdcSecureBridge.bytes(1, 0, Uint8Array.of(77)); await settle();
        assert.equal(f.transport().connectionState, "connected");
    } finally { owner.close(); }
});

test("incoming fails on signaling loss before authentication or invalid records afterwards", async () => {
    const f = fixture(), first = await readyListener(f);
    first.listener.mailbox.fail(new ProxyError("stream_closed", "offline")); await settle();
    assert.equal(first.state, "failed"); first.close();
    const g = fixture(), second = await readyListener(g);
    g.emit([1]); await settle(); assert.equal(await second.accept(), true);
    g.transport().connect(); await settle(); g.module.p2pdcSecureBridge.number(1, 0, 4096); await settle();
    second.listener.mailbox.fail(new ProxyError("invalid_record", "invalid")); await settle();
    assert.equal(second.state, "failed"); second.close();
});
