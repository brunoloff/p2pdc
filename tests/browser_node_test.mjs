import test from "node:test";
import assert from "node:assert/strict";
import { BrowserNode } from "../src/browser/browser_node.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
class FakePeer {
    constructor() { this.connectionState = "new"; this.iceConnectionState = "new"; this.sctp = { maxMessageSize: 65536 }; }
    createDataChannel(label) {
        this.channel = { label, ordered: true, maxPacketLifeTime: null, maxRetransmits: null,
            readyState: "connecting", bufferedAmount: 0, close() { this.readyState = "closed"; }, send() {} };
        return this.channel;
    }
    async createOffer() { return { type: "offer", sdp: "offer" }; }
    async setLocalDescription(value) { this.localDescription = { toJSON: () => value }; }
    async getStats() { return new Map(); }
    close() { this.connectionState = "closed"; }
    connect() { this.connectionState = "connected"; this.iceConnectionState = "connected";
        this.channel.readyState = "open"; this.onconnectionstatechange(); }
}
function fixture() {
    let id = 0, timerId = 0, current = true, currentHandle = 7, known = false, transport, holdLookup = false;
    const events = [], timers = new Map(), listeners = [], lookup = deferred();
    const packed = new Uint8Array(132);
    packed.fill(4, 0, 32); packed.fill(5, 32, 64); packed.fill(6, 64, 80); packed.fill(7, 80, 100);
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 100, _free: () => {},
        _p2pdc_discovery_create: () => ++id, _p2pdc_discovery_destroy: () => {}, _p2pdc_discovery_healthy: () => 1,
        _p2pdc_discovery_key: (_id, ptr, size) => { module.HEAPU8.fill(1, ptr, ptr + size); return 1; },
        _p2pdc_discovery_window_key: (_id, now, ptr, size) => { module.HEAPU8.fill(Math.floor(now / 120) + 1, ptr, ptr + size); return 1; },
        _p2pdc_discovery_self: (_id, ptr, size) => { module.HEAPU8.fill(2, ptr, ptr + size); return 1; },
        _p2pdc_discovery_publish: (_id, _time, ptr, size) => { module.HEAPU8.fill(3, ptr, ptr + size); return 1; },
        _p2pdc_discovery_publish_windowed: (_id, _time, ptr, size) => { module.HEAPU8.fill(3, ptr, ptr + size); return 1; },
        _p2pdc_discovery_receive: (watchId) => { module.p2pdcDiscoveryBridge.event(watchId, currentHandle, 0, packed); return 0; },
        _p2pdc_discovery_sweep: () => 1, _p2pdc_discovery_current: (_id, handle, _time, ptr) => {
            if (!current || handle !== currentHandle) return 0; module.HEAPU8.set(packed, ptr); return 1;
        },
        _p2pdc_discovery_incoming: (_local, _watch, _ptr, _size, _time, _inspect, out) => {
            if (!known) return 3;
            module.HEAPU8.fill(0, out, out + 20); module.HEAPU8[out + 3] = 7;
            module.HEAPU8.fill(9, out + 4, out + 20); return 1;
        },
        _p2pdc_secure_create: () => 1, _p2pdc_secure_destroy: () => {}, _p2pdc_secure_description: () => 0,
        _p2pdc_secure_candidate: () => 0, _p2pdc_secure_receive: () => 0, _p2pdc_secure_description_done: () => 0,
        _p2pdc_secure_candidate_done: () => 0, _p2pdc_secure_ready: () => 0, _p2pdc_secure_frame: () => 0,
        _p2pdc_secure_send: () => 0, _p2pdc_secure_wake: () => 0, _p2pdc_secure_timeout: () => 0 };
    const proxy = { listen: (key, callback) => { const done = deferred(); listeners.push({ key: [...key], callback, done });
        return { ready: Promise.resolve(true), done: done.promise, close: () => done.resolve() }; },
    get: async () => holdLookup ? lookup.promise : [], put: async () => {} };
    const identity = { identitySeed: new Uint8Array(32).fill(8), recipientSecret: new Uint8Array(32).fill(9),
        capability: new Uint8Array(32).fill(10) };
    const timing = { now: () => 1000, timers: { set: (fn, delay) => {
        const key = ++timerId; timers.set(key, { fn, delay }); return key;
    }, clear: (key) => timers.delete(key) } };
    const options = { watch: { ...timing, windowed: false } };
    const handlers = { onPeer: (peer, event) => events.push(["peer", peer, event]),
        onCatchup: () => events.push(["catchup"]), onError: (reason) => events.push(["error", reason]) };
    const attemptOptions = { transport: { relayPolicy: "automatic",
        peerConnectionFactory: () => { transport = new FakePeer(); return transport; } },
    now: () => 1000, lease: timing, listener: { ...timing, deadlineNow: () => 1000000 } };
    const attemptHandlers = { onPublished: () => {}, onRequest: (peer) => events.push(["request", peer]),
        onState: () => {}, onRoute: () => {}, onMessage: () => {}, onWritable: () => {}, onStatus: () => {}, onError: () => {} };
    return { module, proxy, identity, options, handlers, attemptOptions, attemptHandlers, events, timers, listeners,
        emit: (index, bytes) => listeners[index].callback({ bytes: Uint8Array.from(bytes), expired: false }),
        stale: () => { current = false; }, update: () => { currentHandle = 8; },
        known: () => { known = true; }, transport: () => transport,
        holdLookup: () => { holdLookup = true; }, finishLookup: () => lookup.resolve([]) };
}
async function settle() { for (let i = 0; i < 30; ++i) await Promise.resolve(); }
async function started(f) {
    const node = new BrowserNode(f.module, f.proxy, f.identity, f.handlers, f.options);
    assert.equal(await node.start(), true); await settle(); return node;
}

test("node reports verified peers and rejects foreign or stale handles", async () => {
    const f = fixture(), node = await started(f);
    try {
        f.emit(0, new Uint8Array(242)); await settle();
        const peer = f.events.find((event) => event[0] === "peer")[1];
        assert.throws(() => node.connect({ ...peer }, f.attemptOptions, f.attemptHandlers), TypeError);
        f.stale();
        assert.throws(() => node.connect(peer, f.attemptOptions, f.attemptHandlers), /stale/);
        assert.equal(node.state, "running");
    } finally { node.close(); }
    assert.equal(f.timers.size, 0);
});

test("node delivers live discovery only after startup permits connecting", async () => {
    const f = fixture(); f.holdLookup();
    let node, connection;
    f.handlers.onPeer = (peer) => { connection = node.connect(peer, f.attemptOptions, f.attemptHandlers); };
    node = new BrowserNode(f.module, f.proxy, f.identity, f.handlers, f.options);
    const startup = node.start(); await settle();
    f.emit(0, new Uint8Array(242)); await settle();
    assert.equal(node.state, "starting"); assert.equal(connection, undefined);
    f.finishLookup(); assert.equal(await startup, true); await settle();
    assert.equal(node.state, "running"); assert.ok(connection);
    node.close(); await settle(); assert.equal(f.timers.size, 0);
});

test("superseded startup events do not expose stale peer handles", async () => {
    const f = fixture(); f.holdLookup();
    let node, connectedHandle;
    f.handlers.onPeer = (peer) => { connectedHandle = peer.handle; node.connect(peer, f.attemptOptions, f.attemptHandlers); };
    node = new BrowserNode(f.module, f.proxy, f.identity, f.handlers, f.options);
    const startup = node.start(); await settle();
    f.emit(0, new Uint8Array(242)); await settle();
    f.update(); f.emit(0, new Uint8Array(242)); await settle();
    f.finishLookup(); assert.equal(await startup, true); await settle();
    assert.equal(node.state, "running"); assert.equal(connectedHandle, 8);
    node.close(); await settle(); assert.equal(f.timers.size, 0);
});

test("startup retains room for 128 peer events and catch-up", async () => {
    const f = fixture(); f.holdLookup();
    const node = new BrowserNode(f.module, f.proxy, f.identity, f.handlers, f.options);
    const startup = node.start(); await settle();
    const peer = { handle: 7, signingKey: new Uint8Array(32).fill(4), recipientKey: new Uint8Array(32).fill(5),
        instance: new Uint8Array(16).fill(6), inbox: new Uint8Array(20).fill(7) };
    for (let i = 0; i < 128; ++i) node.peer(peer, "seen");
    node.catchup();
    assert.equal(node.state, "starting");
    f.finishLookup(); assert.equal(await startup, true); await settle();
    assert.equal(node.state, "running");
    assert.equal(f.events.filter((event) => event[0] === "peer").length, 128);
    assert.ok(f.events.some((event) => event[0] === "catchup")); node.close();
});

test("peer events delayed by recovery are rechecked on readiness", async () => {
    const f = fixture(), node = await started(f);
    f.emit(0, new Uint8Array(242)); await settle();
    const peer = f.events.find((event) => event[0] === "peer")[1]; f.events.length = 0;
    const current = node.watch.current.bind(node.watch);
    const healthy = node.watch.healthy.bind(node.watch);
    const recovering = node.watch.recovering.bind(node.watch);
    node.watch.current = () => null; node.watch.healthy = () => false; node.watch.recovering = () => true;
    node.peer(peer, "updated"); node.peer(peer, "updated"); await settle();
    assert.equal(f.events.filter((event) => event[0] === "peer").length, 0);
    node.watch.current = current; node.watch.healthy = healthy; node.watch.recovering = recovering;
    node.catchup(); await settle();
    assert.equal(f.events.filter((event) => event[0] === "peer").length, 1);
    node.watch.current = () => null; node.watch.healthy = () => false; node.watch.recovering = () => true;
    node.peer(peer, "updated"); await settle(); f.stale();
    node.watch.current = current; node.watch.healthy = healthy; node.watch.recovering = recovering;
    node.catchup(); await settle();
    assert.equal(f.events.filter((event) => event[0] === "peer").length, 1); node.close();
});

test("rotating node delivers a peer found before the new window catches up", async () => {
    const f = fixture(), pending = deferred(); let now = 1000;
    f.options.watch.windowed = true; f.options.watch.now = () => now;
    f.proxy.get = async (key) => key[0] === 10 ? pending.promise : [];
    const node = await started(f); f.events.length = 0;
    now = 1050; node.watch.tick(); await settle();
    assert.equal(node.watch.recovering(), true);
    f.emit(1, new Uint8Array(242)); await settle();
    assert.equal(f.events.filter((event) => event[0] === "peer").length, 0);
    pending.resolve([]); await settle();
    assert.equal(node.watch.healthy(), true);
    assert.equal(f.events.filter((event) => event[0] === "peer").length, 1);
    node.close();
});

test("node propagates its rotating protocol and rejects conflicting announcement options", async () => {
    const f = fixture(); f.options.watch.windowed = true;
    const node = await started(f);
    f.emit(0, new Uint8Array(242)); await settle();
    const peer = f.events.find((event) => event[0] === "peer")[1];
    assert.throws(() => node.connect(peer, { ...f.attemptOptions, lease: { windowed: false } }, f.attemptHandlers),
        /protocols must match/);
    assert.throws(() => node.listen({ ...f.attemptOptions, listener: { windowed: false } }, f.attemptHandlers),
        /protocols must match/);
    const listener = node.listen(f.attemptOptions, f.attemptHandlers);
    assert.equal(listener.listener.lease.windowed, true);
    await settle(); assert.equal(listener.state, "listening"); node.close();
});

test("browser node defaults to rotating discovery and permits explicit legacy mode", async () => {
    const f = fixture(); delete f.options.watch.windowed;
    const node = await started(f);
    assert.equal(node.watch.windowed, true);
    const listener = node.listen(f.attemptOptions, f.attemptHandlers);
    assert.equal(listener.listener.lease.windowed, true);
    await settle(); assert.equal(listener.state, "listening"); node.close();
    const g = fixture(), legacy = await started(g);
    assert.equal(legacy.watch.windowed, false); legacy.close();
});

test("closing a node suppresses peers deferred during recovery", async () => {
    const f = fixture(), node = await started(f);
    f.emit(0, new Uint8Array(242)); await settle();
    const peer = f.events.find((event) => event[0] === "peer")[1]; f.events.length = 0;
    node.watch.current = () => null; node.watch.healthy = () => false; node.watch.recovering = () => true;
    node.peer(peer, "updated"); await settle(); node.close(); node.catchup(); await settle();
    assert.deepEqual(f.events, []);
});

test("node retries unknown incoming offers after discovery changes", async () => {
    const f = fixture(), node = await started(f);
    try {
        const listener = node.listen(f.attemptOptions, f.attemptHandlers); await settle();
        assert.equal(listener.state, "listening");
        f.emit(1, [1]); await settle();
        assert.equal(f.events.some((event) => event[0] === "request"), false);
        f.known(); f.emit(0, new Uint8Array(242)); await settle();
        assert.equal(f.events.filter((event) => event[0] === "request").length, 1);
        assert.equal(listener.state, "pending");
    } finally { node.close(); }
    assert.equal(f.timers.size, 0);
});

test("node bounds attempts and close cancels owned handles", async () => {
    const f = fixture(), node = await started(f), handles = [];
    for (let i = 0; i < 8; ++i) handles.push(node.listen(f.attemptOptions, f.attemptHandlers));
    assert.throws(() => node.listen(f.attemptOptions, f.attemptHandlers), RangeError);
    handles[0].close();
    handles.push(node.listen(f.attemptOptions, f.attemptHandlers));
    node.close(); await settle();
    assert.ok(handles.every((handle) => handle.state === "closed"));
    assert.equal(f.timers.size, 0);
    assert.throws(() => node.listen(f.attemptOptions, f.attemptHandlers), /not ready/);
});

test("discovery failure preserves an authenticated connection until node close", async () => {
    const f = fixture(), node = await started(f);
    f.emit(0, new Uint8Array(242)); await settle();
    const peer = f.events.find((event) => event[0] === "peer")[1];
    const connection = node.connect(peer, f.attemptOptions, f.attemptHandlers); await settle();
    assert.equal(connection.maxMessageSize(), null);
    f.transport().connect(); await settle(); f.module.p2pdcSecureBridge.number(1, 0, 262144); await settle();
    assert.equal(connection.state, "ready");
    assert.equal(connection.maxMessageSize(), 262144);
    f.listeners[0].done.reject(new Error("offline")); await settle();
    assert.equal(node.state, "failed"); assert.equal(connection.state, "ready");
    assert.equal(connection.send(Uint8Array.of(1)), "accepted");
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "backend"]]);
    node.close(); assert.equal(connection.state, "closed"); assert.equal(f.timers.size, 0);
    assert.equal(connection.maxMessageSize(), null);
});

test("node reports discovery recovery and blocks new attempts during the outage", async () => {
    const f = fixture();
    f.handlers.onStatus = (status) => f.events.push(["status", status]);
    const node = await started(f);
    f.listeners[0].done.reject(new ProxyError("stream_closed", "offline")); await settle();
    assert.equal(node.state, "running");
    assert.throws(() => node.listen(f.attemptOptions, f.attemptHandlers), /not ready/);
    assert.deepEqual(f.events.filter((event) => event[0] === "status"), [["status", "reconnecting"]]);
    const retry = [...f.timers.entries()].find(([, timer]) => timer.delay === 1000);
    assert.ok(retry); f.timers.delete(retry[0]); retry[1].fn(); await settle();
    assert.deepEqual(f.events.filter((event) => event[0] === "status"),
        [["status", "reconnecting"], ["status", "ready"]]);
    const incoming = node.listen(f.attemptOptions, f.attemptHandlers);
    assert.ok(incoming);
    node.close(); await settle(); assert.equal(f.timers.size, 0);
});

test("pending node listener can be accepted after discovery recovers", async () => {
    const f = fixture(), node = await started(f);
    const listener = node.listen(f.attemptOptions, f.attemptHandlers); await settle();
    f.known(); f.emit(1, [1]); await settle(); assert.equal(listener.state, "pending");
    node.watch.mailbox.current.fail(new ProxyError("stream_closed", "offline"));
    assert.equal(await listener.accept(), false); assert.equal(listener.state, "pending");
    await settle();
    assert.equal(await listener.accept(), false); assert.equal(listener.state, "pending");
    const retry = [...f.timers.entries()].find(([, timer]) => timer.delay === 1000);
    assert.ok(retry); f.timers.delete(retry[0]); retry[1].fn(); await settle();
    assert.equal(await listener.accept(), true); assert.equal(listener.state, "connecting");
    node.close(); await settle(); assert.equal(f.timers.size, 0);
});
