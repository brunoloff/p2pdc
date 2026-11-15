import test from "node:test";
import assert from "node:assert/strict";
import { BrowserConnection } from "../src/browser/browser_connection.mjs";

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
    connect() {
        this.connectionState = "connected"; this.iceConnectionState = "connected";
        this.channel.readyState = "open"; this.onconnectionstatechange();
    }
}
function fixture() {
    let destroyed = 0, sends = 0, peer;
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 128, _free: () => {},
        _p2pdc_secure_create: () => 1, _p2pdc_secure_destroy: () => { ++destroyed; },
        _p2pdc_secure_description: () => 0, _p2pdc_secure_candidate: () => 0, _p2pdc_secure_receive: () => 0,
        _p2pdc_secure_description_done: () => 0, _p2pdc_secure_candidate_done: () => 0,
        _p2pdc_secure_ready: () => 0, _p2pdc_secure_frame: () => 0, _p2pdc_secure_send: () => { ++sends; return 0; },
        _p2pdc_secure_wake: () => 0, _p2pdc_secure_timeout: () => 0 };
    const secure = { role: "offerer", timeoutMs: 10000, identitySeed: new Uint8Array(32).fill(1),
        recipientSecret: new Uint8Array(32).fill(2), capability: new Uint8Array(32).fill(3),
        remoteSigningKey: new Uint8Array(32).fill(4), remoteBoxKey: new Uint8Array(32).fill(5),
        localInstance: new Uint8Array(16).fill(6), remoteInstance: new Uint8Array(16).fill(7),
        attemptId: new Uint8Array(16).fill(8) };
    const options = { secure, transport: { role: "offerer", relayPolicy: "automatic",
        peerConnectionFactory: () => { peer = new FakePeer(); return peer; } }, now: () => 1000 };
    const events = [], handlers = { onSignal: (v) => events.push(["signal", v]), onState: (v) => events.push(["state", v]),
        onRoute: (v) => events.push(["route", v]), onMessage: (v) => events.push(["message", v]),
        onWritable: () => events.push(["writable"]), onError: (v) => events.push(["error", v]) };
    return { module, options, handlers, events, peer: () => peer, destroyed: () => destroyed, sends: () => sends };
}
async function settle() { for (let i = 0; i < 12; ++i) await Promise.resolve(); }

test("connection waits for authentication and delivers secure messages", async () => {
    const f = fixture(), connection = new BrowserConnection(f.module, f.options, f.handlers);
    try {
        assert.equal(connection.send(Uint8Array.of(1)), "notReady");
        assert.equal(connection.maxMessageSize(), null);
        await connection.start(); f.peer().connect(); await settle();
        assert.equal(connection.state, "connecting");
        f.module.p2pdcSecureBridge.number(1, 0, 262144); await settle();
        assert.equal(connection.state, "ready");
        assert.equal(connection.maxMessageSize(), 262144);
        assert.equal(connection.send(Uint8Array.of(1)), "accepted");
        f.module.p2pdcSecureBridge.bytes(1, 2, Uint8Array.of(0, 255)); await settle();
        assert.deepEqual([...f.events.find((event) => event[0] === "message")[1]], [0, 255]);
        assert.equal(connection.route().current, "unknown");
    } finally { connection.close(); }
    assert.equal(f.peer().connectionState, "closed"); assert.equal(f.destroyed(), 1);
    assert.equal(connection.send(Uint8Array.of(1)), "closed");
    assert.equal(connection.maxMessageSize(), null);
});

test("negotiated limit survives suspension and invalid limits fail closed", async () => {
    const f = fixture(), connection = new BrowserConnection(f.module, f.options, f.handlers);
    await connection.start(); f.peer().connect(); await settle();
    f.module.p2pdcSecureBridge.number(1, 0, 1024); await settle();
    assert.equal(connection.maxMessageSize(), 1024);
    f.peer().connectionState = "disconnected"; f.peer().iceConnectionState = "disconnected";
    f.peer().onconnectionstatechange(); await settle();
    assert.equal(connection.state, "suspended"); assert.equal(connection.maxMessageSize(), 1024);
    connection.close(); assert.equal(connection.maxMessageSize(), null);
    const g = fixture(), invalid = new BrowserConnection(g.module, g.options, g.handlers);
    await invalid.start(); g.peer().connect(); await settle();
    g.module.p2pdcSecureBridge.number(1, 0, 0); await settle();
    assert.equal(invalid.state, "failed"); assert.equal(invalid.maxMessageSize(), null);
    invalid.close();
    const h = fixture(); h.options.secure.maxMessageBytes = 1000;
    const tooLarge = new BrowserConnection(h.module, h.options, h.handlers);
    await tooLarge.start(); h.peer().connect(); await settle();
    h.module.p2pdcSecureBridge.number(1, 0, 1001); await settle();
    assert.equal(tooLarge.state, "failed"); tooLarge.close();
});

test("asymmetric negotiated limits reject oversize sends before copying to WebAssembly", async () => {
    const f = fixture(); f.options.secure.maxMessageBytes = 4 * 1024 * 1024;
    const connection = new BrowserConnection(f.module, f.options, f.handlers);
    await connection.start(); f.peer().connect(); await settle();
    f.module.p2pdcSecureBridge.number(1, 0, 8192); await settle();
    assert.equal(connection.send(new Uint8Array(8193)), "tooLarge");
    assert.equal(f.sends(), 0);
    assert.equal(connection.send(new Uint8Array(8192)), "accepted");
    assert.equal(f.sends(), 1);
    assert.throws(() => connection.send({ byteLength: 8193 }), TypeError);
    connection.close();
});

test("terminal secure failure closes transport and suppresses late callbacks", async () => {
    const f = fixture(), connection = new BrowserConnection(f.module, f.options, f.handlers);
    await connection.start();
    f.module.p2pdcSecureBridge.number(1, 2, 1); await settle();
    assert.equal(connection.state, "failed"); assert.equal(f.peer().connectionState, "closed");
    assert.deepEqual(f.events.filter((event) => event[0] === "state"), [["state", "failed"]]);
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "protocol"]]);
    assert.ok(f.events.findIndex((event) => event[0] === "state") <
        f.events.findIndex((event) => event[0] === "error"));
    assert.equal(connection.send(Uint8Array.of(1)), "failed");
    connection.close(); await settle();
    assert.equal(f.destroyed(), 2); // the Wasm export's destroy operation is idempotent
});

test("invalid setup and clock failures fail closed", async () => {
    const f = fixture();
    assert.throws(() => new BrowserConnection(f.module, { ...f.options, transport: { role: "answerer" } }, f.handlers), TypeError);
    for (const field of ["maxMessageBytes", "maxBufferBytes", "maxInboundBytes"]) {
        assert.throws(() => new BrowserConnection(f.module,
            { ...f.options, transport: { ...f.options.transport, [field]: 1023 } }, f.handlers), RangeError);
    }
    const connection = new BrowserConnection(f.module, { ...f.options, now: () => -1 }, f.handlers);
    await connection.start();
    assert.equal(connection.receive(Uint8Array.of(1)), "failed"); await settle();
    assert.equal(connection.state, "failed"); assert.equal(f.peer().connectionState, "closed");
    connection.close();
});

test("signaling callback errors fail the attempt and close suppresses pending notifications", async () => {
    const f = fixture();
    f.handlers.onSignal = () => { throw new Error("outbox full"); };
    const connection = new BrowserConnection(f.module, f.options, f.handlers);
    await connection.start();
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 0, Uint8Array.of(1)), true);
    await settle();
    assert.equal(connection.state, "failed");
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "callback"]]);
    connection.close();
    const g = fixture(), other = new BrowserConnection(g.module, g.options, g.handlers);
    await other.start();
    g.module.p2pdcSecureBridge.bytes(1, 0, Uint8Array.of(1));
    other.close(); await settle();
    assert.equal(g.events.some((event) => event[0] === "signal"), false);
    await assert.rejects(other.start());
});

test("candidate without a MID fails and closes the connection", async () => {
    const f = fixture(), connection = new BrowserConnection(f.module, f.options, f.handlers);
    await connection.start();
    f.peer().onicecandidate({ candidate: { candidate: "candidate:test", toJSON: () =>
        ({ candidate: "candidate:test", sdpMLineIndex: 0 }) } });
    await settle();
    assert.equal(connection.state, "failed");
    assert.deepEqual(f.events.filter((event) => event[0] === "error"), [["error", "candidate"]]);
    connection.close();
});
