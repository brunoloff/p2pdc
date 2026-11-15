import test from "node:test";
import assert from "node:assert/strict";
import { createWebRtcBridge } from "../src/browser/webrtc_bridge.mjs";

function setup() {
    const events = [], transports = [];
    const bridge = createWebRtcBridge((...event) => { events.push(event); return true; }, (options, handlers) => {
        const transport = { options, handlers, closed: false, started: 0, sent: [],
            startOffer() { ++this.started; return Promise.resolve(); },
            setRemoteDescription(value) { this.remote = value; return Promise.resolve(); },
            addRemoteCandidate(value) { this.candidate = value; return Promise.resolve(); },
            send(value) { this.sent.push(value); return "accepted"; },
            bufferedAmount() { return 13; }, maxMessageSize() { return 65536; },
            pollRoute() { return Promise.resolve(); }, close() { this.closed = true; } };
        transports.push(transport); return transport;
    });
    const id = bridge.begin("offerer", "automatic", {});
    return { bridge, id, events, transports };
}
const settle = async () => { for (let i = 0; i < 5; ++i) await Promise.resolve(); };

test("bridges transport data, signaling and selected route", async () => {
    const { bridge, id, events, transports } = setup();
    bridge.addServer(id, "turn:example.com:3478?transport=tcp", "user", "secret");
    bridge.open(id); const transport = transports[0];
    assert.equal(transport.options.iceServers[0].credential, "secret");
    bridge.start(id); assert.equal(transport.started, 1);
    bridge.description(id, "remote"); bridge.candidate(id, 9, "candidate", "0");
    await settle();
    assert.deepEqual(transport.remote, { type: "answer", sdp: "remote" });
    assert.deepEqual(transport.candidate, { candidate: "candidate", sdpMid: "0" });
    assert.ok(events.some((event) => event[1] === 7 && event[2] === 1));
    assert.ok(events.some((event) => event[1] === 8 && event[2] === 9));
    transport.handlers.onDescription({ sdp: "local" });
    transport.handlers.onCandidate({ candidate: "candidate", sdpMid: "0" });
    transport.handlers.onState("ready"); transport.handlers.onBinary(Uint8Array.of(0, 255));
    transport.handlers.onRoute({ current: "turnRelayed", turnHistory: "used",
        selectedPair: { local: "relayed", remote: "host" } });
    assert.equal(events.find((event) => event[1] === 4)[2], 2 | 4 | 8 | (4 << 4) | (1 << 8));
    assert.equal(bridge.send(id, Uint8Array.of(42)), 0);
    assert.equal(bridge.buffered(id), 13); assert.equal(bridge.maximum(id), 65536);
    bridge.poll(id); bridge.close(id); assert.equal(transport.closed, true);
    assert.throws(() => bridge.send(id, Uint8Array.of(1)), TypeError);
});

test("rejects duplicate operations and suppresses late completion after close", async () => {
    const { bridge, id, events, transports } = setup();
    bridge.open(id); const transport = transports[0];
    let complete;
    transport.setRemoteDescription = () => new Promise((resolve) => { complete = resolve; });
    bridge.description(id, "x");
    assert.throws(() => bridge.description(id, "x"), TypeError);
    bridge.close(id); complete(); await settle();
    assert.equal(events.length, 0);
    bridge.close(id);
});

test("failed applications and delivery are reported without leaking transport state", async () => {
    const { bridge, id, events, transports } = setup();
    bridge.open(id); const transport = transports[0];
    transport.addRemoteCandidate = () => Promise.reject(new Error("rejected"));
    bridge.candidate(id, 7, "bad", "0"); await settle();
    assert.ok(events.some((event) => event[1] === 9 && event[2] === 7));
    transport.handlers.onCandidate({ candidate: "bad", sdpMid: null });
    assert.ok(events.some((event) => event[1] === 5));
    bridge.close(id);
    let closed = false, handlers;
    const failing = createWebRtcBridge(() => false, (_options, value) => {
        handlers = value; return { close() { closed = true; } };
    });
    const key = failing.begin("offerer", "automatic", {}); failing.open(key);
    handlers.onState("ready"); assert.equal(closed, true);
});

test("bounds outstanding candidates and transport instances", () => {
    const { bridge, id, transports } = setup();
    bridge.open(id); transports[0].addRemoteCandidate = () => new Promise(() => {});
    for (let i = 1; i <= 64; ++i) bridge.candidate(id, i, "candidate", "0");
    assert.throws(() => bridge.candidate(id, 65, "candidate", "0"), TypeError);
    assert.throws(() => bridge.candidate(id, 1, "candidate", "0"), TypeError);
    for (let i = 1; i < 256; ++i) bridge.begin("offerer", "automatic", {});
    assert.throws(() => bridge.begin("offerer", "automatic", {}), RangeError);
    bridge.close(id);
});
