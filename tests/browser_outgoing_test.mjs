import test from "node:test";
import assert from "node:assert/strict";
import { BrowserOutgoing } from "../src/browser/browser_outgoing.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
class FakePeer {
    constructor(descriptionGate) { this.descriptionGate = descriptionGate; this.connectionState = "new";
        this.iceConnectionState = "new"; this.sctp = { maxMessageSize: 65536 }; }
    createDataChannel(label) {
        this.channel = { label, ordered: true, maxPacketLifeTime: null, maxRetransmits: null,
            readyState: "connecting", bufferedAmount: 0, close() { this.readyState = "closed"; }, send() {} };
        return this.channel;
    }
    async createOffer() { return { type: "offer", sdp: "offer" }; }
    async setLocalDescription(value) { if (this.descriptionGate) await this.descriptionGate;
        this.localDescription = { toJSON: () => value }; }
    async getStats() { return new Map(); }
    close() { this.connectionState = "closed"; }
    connect() { this.connectionState = "connected"; this.iceConnectionState = "connected";
        this.channel.readyState = "open"; this.onconnectionstatechange(); }
}
function fixture() {
    let current = true, peerConnection, captured, destroyed = 0, nextTimer = 0, descriptionGate, signalFailure = false;
    const events = [], records = [], writes = [], timers = new Map(), publication = deferred(), opened = deferred(), done = deferred();
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 100, _free: () => {},
        _p2pdc_discovery_create: () => 1, _p2pdc_discovery_destroy: () => {}, _p2pdc_discovery_healthy: () => 1,
        _p2pdc_discovery_key: (_id, ptr, size) => { module.HEAPU8.fill(1, ptr, ptr + size); return 1; },
        _p2pdc_discovery_self: (_id, ptr, size) => { module.HEAPU8.fill(2, ptr, ptr + size); return 1; },
        _p2pdc_discovery_publish: (_id, _time, ptr, size) => { module.HEAPU8.fill(3, ptr, ptr + size); return 1; },
        _p2pdc_discovery_receive: () => 1, _p2pdc_discovery_sweep: () => 1, _p2pdc_discovery_current: () => 1,
        _p2pdc_secure_create: (ptr, size) => { captured = module.HEAPU8.slice(ptr, ptr + size); return 1; },
        _p2pdc_secure_destroy: () => { ++destroyed; }, _p2pdc_secure_description: () => 0,
        _p2pdc_secure_candidate: () => 0, _p2pdc_secure_receive: (_id, ptr, size) => {
            records.push([...module.HEAPU8.slice(ptr, ptr + size)]); return 0;
        }, _p2pdc_secure_description_done: () => 0,
        _p2pdc_secure_candidate_done: () => 0, _p2pdc_secure_ready: () => 0, _p2pdc_secure_frame: () => 0,
        _p2pdc_secure_send: () => 0, _p2pdc_secure_wake: () => 0, _p2pdc_secure_timeout: () => 0 };
    const peer = { handle: 7, signingKey: new Uint8Array(32).fill(4), recipientKey: new Uint8Array(32).fill(5),
        instance: new Uint8Array(16).fill(6), inbox: new Uint8Array(20).fill(7) };
    const watch = { windowed: false, healthy: () => true, current: () => current ? peer : null,
        discovery: { module, key: () => new Uint8Array(20).fill(1), self: () => ({
            signingKey: new Uint8Array(32).fill(2), recipientKey: new Uint8Array(32).fill(2) }) } };
    const proxy = { listen: () => ({ ready: opened.promise, done: done.promise, close: () => done.resolve() }),
        get: async () => [], put: (_key, wire) => {
            writes.push([...wire]); return signalFailure ? Promise.reject(new Error("offline")) : publication.promise;
        } };
    const identity = { identitySeed: new Uint8Array(32).fill(8), recipientSecret: new Uint8Array(32).fill(9),
        capability: new Uint8Array(32).fill(10) };
    const options = { now: () => 1000, transport: { relayPolicy: "automatic",
        peerConnectionFactory: () => { peerConnection = new FakePeer(descriptionGate); return peerConnection; } },
    lease: { now: () => 1000, windowed: false, timers: { set: (callback, delay) => {
        const id = ++nextTimer; timers.set(id, { callback, delay }); return id;
    }, clear: (id) => timers.delete(id) } } };
    const handlers = { onState: (v) => events.push(["state", v]), onRoute: (v) => events.push(["route", v]),
        onMessage: (v) => events.push(["message", v]), onWritable: () => events.push(["writable"]),
        onStatus: (v) => events.push(["status", v]), onError: (v) => events.push(["error", v]) };
    return { module, peer, watch, proxy, identity, options, handlers, events, records, writes, publication, opened, done, timers,
        stale: () => { current = false; }, transport: () => peerConnection, captured: () => captured,
        holdDescription: (value) => { descriptionGate = value; }, failSignal: () => { signalFailure = true; },
        destroyed: () => destroyed };
}
async function settle() { for (let i = 0; i < 20; ++i) await Promise.resolve(); }
async function createAndStart(f) {
    const owner = new BrowserOutgoing(f.module, f.proxy, f.identity, f.watch, f.peer, f.options, f.handlers);
    const start = owner.start(); f.opened.resolve(true); assert.equal(await start, true); await settle(); return owner;
}

test("outgoing rejects invalid application limits before starting discovery", () => {
    const f = fixture(); f.options.maxMessageBytes = 4194305;
    assert.throws(() => new BrowserOutgoing(f.module, f.proxy, f.identity, f.watch, f.peer, f.options, f.handlers), RangeError);
    assert.equal(f.timers.size, 0); assert.equal(f.transport(), undefined);
});

test("outgoing rejects a watch using a different discovery protocol", () => {
    const f = fixture(); f.watch.windowed = true;
    assert.throws(() => new BrowserOutgoing(f.module, f.proxy, f.identity, f.watch, f.peer, f.options, f.handlers),
        /do not match/);
    assert.equal(f.timers.size, 0);
});

test("outgoing owner copies credentials, waits for publication and gates messages", async () => {
    const f = fixture(); f.options.maxMessageBytes = 4096;
    const owner = await createAndStart(f);
    try {
        assert.equal(owner.send(Uint8Array.of(1)), "notReady");
        f.identity.identitySeed.fill(0); f.identity.recipientSecret.fill(0); f.identity.capability.fill(0);
        f.publication.resolve(); await settle();
        assert.equal(owner.state, "connecting");
        assert.equal(f.captured()[0], 8); assert.equal(f.captured()[32], 9); assert.equal(f.captured()[64], 10);
        assert.equal(new DataView(f.captured().buffer).getUint32(209), 4096);
        assert.equal(owner.identity, null);
        f.transport().connect(); await settle();
        f.module.p2pdcSecureBridge.number(1, 0, 4096); await settle();
        assert.equal(owner.state, "ready"); assert.equal(owner.send(Uint8Array.of(4)), "accepted");
        assert.equal(f.timers.size, 0);
        assert.equal(owner.lease.closed, true);
        assert.equal(owner.route().current, "unknown");
    } finally { owner.close(); }
    assert.equal(f.transport().connectionState, "closed"); assert.equal(f.destroyed(), 1);
    assert.equal(f.timers.size, 0); assert.equal(owner.send(Uint8Array.of(1)), "closed");
});

test("stale peer fails before creating the transport", async () => {
    const f = fixture(), owner = await createAndStart(f);
    f.stale(); f.publication.resolve(); await settle();
    assert.equal(owner.state, "failed"); assert.equal(f.transport(), undefined);
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "stalePeer"]]);
    assert.equal(owner.identity, null); owner.close();
});

test("close during inbox startup prevents publication and callbacks", async () => {
    const f = fixture(), owner = new BrowserOutgoing(f.module, f.proxy, f.identity, f.watch, f.peer, f.options, f.handlers);
    const start = owner.start(); owner.close(); f.opened.resolve(true);
    assert.equal(await start, false); await settle();
    assert.equal(owner.state, "closed"); assert.deepEqual(f.events, []); assert.equal(f.timers.size, 0);
});

test("mismatched identity and bounded early signaling fail closed", async () => {
    const f = fixture(); f.watch.discovery.key = () => new Uint8Array(20).fill(9);
    assert.throws(() => new BrowserOutgoing(f.module, f.proxy, f.identity, f.watch, f.peer, f.options, f.handlers));
    const invalid = fixture();
    invalid.options.transport.maxMessageBytes = 1023;
    assert.throws(() => new BrowserOutgoing(invalid.module, invalid.proxy, invalid.identity, invalid.watch,
        invalid.peer, invalid.options, invalid.handlers), RangeError);
    const g = fixture(), owner = await createAndStart(g);
    for (let i = 0; i < 65; ++i) owner.receive(Uint8Array.of(i));
    await settle(); assert.equal(owner.state, "failed");
    assert.deepEqual(g.events.filter((event) => event[0] === "error"), [["error", "backpressure"]]);
    owner.close();
});

test("outgoing owner preserves early records until transport startup finishes", async () => {
    const f = fixture(), gate = deferred(); f.holdDescription(gate.promise);
    const owner = await createAndStart(f);
    try {
        f.publication.resolve(); await settle();
        owner.receive(Uint8Array.of(23)); await settle();
        assert.deepEqual(f.records, []);
        gate.resolve(); await settle();
        assert.deepEqual(f.records, [[23]]);
    } finally { owner.close(); }
});

test("outgoing signaling publication failure closes the connection", async () => {
    const f = fixture(), owner = await createAndStart(f);
    f.publication.resolve(); await settle();
    f.failSignal();
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 0, Uint8Array.of(77)), true);
    await settle();
    assert.ok(f.writes.some((wire) => wire.length === 1 && wire[0] === 77));
    assert.equal(owner.state, "failed");
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "signaling"]]);
    assert.equal(f.transport().connectionState, "closed"); owner.close();
});

test("outgoing retains an authenticated channel when signaling disconnects", async () => {
    const f = fixture(), owner = await createAndStart(f);
    try {
        f.publication.resolve(); await settle(); f.transport().connect(); await settle();
        f.module.p2pdcSecureBridge.number(1, 0, 4096); await settle();
        f.done.reject(new ProxyError("stream_closed", "offline")); await settle();
        assert.equal(owner.state, "ready");
        assert.equal(owner.send(Uint8Array.of(3)), "accepted");
        assert.deepEqual(f.events.filter((e) => e[0] === "status"), [["status", "signalingUnavailable"]]);
        const count = f.writes.length;
        f.module.p2pdcSecureBridge.bytes(1, 0, Uint8Array.of(77)); await settle();
        assert.equal(f.writes.length, count); assert.equal(f.transport().connectionState, "connected");
    } finally { owner.close(); }
});

test("outgoing fails on signaling loss before authentication or invalid records afterwards", async () => {
    const f = fixture(), first = await createAndStart(f);
    f.done.reject(new ProxyError("stream_closed", "offline")); await settle();
    assert.equal(first.state, "failed"); first.close();
    const g = fixture(), second = await createAndStart(g);
    g.publication.resolve(); await settle(); g.transport().connect(); await settle();
    g.module.p2pdcSecureBridge.number(1, 0, 4096); await settle();
    g.done.reject(new ProxyError("invalid_record", "invalid")); await settle();
    assert.equal(second.state, "failed"); second.close();
});
