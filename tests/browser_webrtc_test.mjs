import test from "node:test";
import assert from "node:assert/strict";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";
import { routeKind, selectedPair } from "../src/browser/webrtc_route.mjs";

class FakeChannel {
    constructor(label = "p2pdc/reliable/v1") {
        this.label = label; this.ordered = true; this.maxPacketLifeTime = null;
        this.maxRetransmits = null; this.readyState = "connecting"; this.bufferedAmount = 0;
        this.sent = [];
    }
    send(value) { this.sent.push(value); }
    close() { this.readyState = "closed"; }
}
function stats(local = "host", remote = "host") {
    return new Map([
        ["transport", { type: "transport", selectedCandidatePairId: "pair" }],
        ["pair", { type: "candidate-pair", localCandidateId: "local", remoteCandidateId: "remote" }],
        ["local", { type: "local-candidate", candidateType: local }],
        ["remote", { type: "remote-candidate", candidateType: remote }],
    ]);
}
class FakePeer {
    constructor(config) {
        this.config = config; this.connectionState = "new"; this.iceConnectionState = "new";
        this.sctp = { maxMessageSize: 65536 }; this.report = stats(); this.candidates = [];
    }
    createDataChannel(name) { this.channel = new FakeChannel(name); return this.channel; }
    async createOffer() { return { type: "offer", sdp: "offer" }; }
    async createAnswer() { return { type: "answer", sdp: "answer" }; }
    async setLocalDescription(value) { this.localDescription = { toJSON: () => value }; }
    async setRemoteDescription(value) { this.remoteDescription = value; }
    async addIceCandidate(value) { this.candidates.push(value); }
    async getStats() { if (this.statsError) throw this.statsError; return this.report; }
    close() { this.connectionState = "closed"; }
    connect() {
        this.connectionState = "connected"; this.iceConnectionState = "connected";
        this.channel.readyState = "open"; this.onconnectionstatechange();
    }
}
function setup(options = {}) {
    const events = { descriptions: [], candidates: [], states: [], bytes: [], routes: [], errors: [], writable: 0, order: [] };
    let peer;
    const handlers = { onDescription: (v) => events.descriptions.push(v), onCandidate: (v) => events.candidates.push(v),
                       onState: (v) => { events.states.push(v); events.order.push(v); },
                       onBinary: (v) => { events.bytes.push(v); events.order.push("binary"); },
                       onRoute: (v) => events.routes.push(v), onError: (v) => events.errors.push(v),
                       onWritable: () => ++events.writable };
    const transport = new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "automatic", ...options,
        peerConnectionFactory: (config) => { peer = new FakePeer(config); return peer; } }, handlers);
    return { transport, peer, events };
}
async function settle() { for (let i = 0; i < 5; ++i) await Promise.resolve(); }

test("selected-pair lookup never guesses from an unselected pair", () => {
    assert.deepEqual(selectedPair(stats("host", "relay")), { local: "host", remote: "relayed" });
    assert.equal(routeKind(selectedPair(stats("host", "relay"))), "turnRelayed");
    const report = stats(); report.delete("transport");
    assert.equal(selectedPair(report), null);
    report.set("a", { type: "transport", selectedCandidatePairId: "pair" });
    report.set("b", { type: "transport", selectedCandidatePairId: "other" });
    assert.equal(selectedPair(report), null);
    assert.equal(routeKind({ local: "unknown", remote: "host" }), "unknown");
});

test("peer-reflexive candidates do not prove a direct route", () => {
    assert.equal(routeKind({ local: "host", remote: "peerReflexive" }), "unknown");
    assert.equal(routeKind({ local: "peerReflexive", remote: "host" }), "unknown");
    assert.equal(routeKind({ local: "relayed", remote: "peerReflexive" }), "turnRelayed");
    assert.equal(routeKind({ local: "host", remote: "serverReflexive" }), "direct");
});

test("offer, early candidates, readiness and binary sends are ordered", async () => {
    const { transport, peer, events } = setup();
    await transport.startOffer();
    await transport.addRemoteCandidate({ candidate: "candidate:one", sdpMid: "0" });
    assert.equal(peer.candidates.length, 0);
    await transport.setRemoteDescription({ type: "answer", sdp: "answer" });
    assert.equal(peer.candidates.length, 1);
    peer.connect();
    peer.channel.onmessage({ data: Uint8Array.of(0, 255).buffer });
    await settle();
    assert.equal(transport.status, "ready");
    assert.deepEqual(events.states, ["ready"]);
    assert.deepEqual([...events.bytes[0]], [0, 255]);
    assert.equal(transport.send(Uint8Array.of(42)), "accepted");
    assert.deepEqual([...peer.channel.sent[0]], [42]);
    assert.equal(events.descriptions[0].type, "offer");
    transport.close();
    assert.equal(transport.send(Uint8Array.of(1)), "closed");
});

test("local candidates wait for the local description for both roles", async () => {
    for (const role of ["offerer", "answerer"]) {
        const { transport, peer, events } = setup({ role });
        const signalingOrder = [];
        transport.handlers.onDescription = (value) => { events.descriptions.push(value); signalingOrder.push("description"); };
        transport.handlers.onCandidate = (value) => { events.candidates.push(value); signalingOrder.push("candidate"); };
        let finish;
        peer.setLocalDescription = (value) => new Promise((resolve) => {
            peer.localDescription = { toJSON: () => value };
            finish = resolve;
        });
        const operation = role === "offerer" ? transport.startOffer() :
            transport.setRemoteDescription({ type: "offer", sdp: "offer" });
        await settle();
        const value = { candidate: "candidate:early", sdpMid: "0" };
        peer.onicecandidate({ candidate: { candidate: value.candidate, toJSON: () => value } });
        assert.deepEqual(events.candidates, []);
        finish(); await operation; await settle();
        assert.equal(events.descriptions.length, 1);
        assert.equal(events.candidates.length, 1);
        assert.deepEqual(signalingOrder, ["description", "candidate"]);
        assert.equal(events.candidates[0].candidate, "candidate:early");
        transport.close();
    }
});

test("relay-only mode signals only redacted relay candidates and filtered descriptions", async () => {
    const iceServers = [{ urls: "turn:relay.example", username: "user", credential: "password" }];
    const { transport, peer, events } = setup({ relayPolicy: "requireLocalRelay", iceServers });
    peer.createOffer = async () => ({ type: "offer", sdp: "c=IN IP4 10.0.0.2\r\na=candidate:1 1 udp 9 10.0.0.2 4 typ host" });
    await transport.startOffer(); await settle();
    const signal = (candidate) => peer.onicecandidate({ candidate: { candidate, toJSON: () => ({ candidate, sdpMid: "0" }) } });
    signal("candidate:1 1 udp 9 10.0.0.2 4 typ host");
    signal("candidate:2 1 udp 8 198.51.100.9 3478 typ relay raddr 10.0.0.2 rport 4");
    await settle();
    assert.equal(events.descriptions.length, 1);
    assert.doesNotMatch(events.descriptions[0].sdp, /10\.0\.0\.2/);
    assert.deepEqual(events.candidates.map((value) => value.candidate),
                     ["candidate:2 1 udp 8 198.51.100.9 3478 typ relay raddr 0.0.0.0 rport 0"]);
    assert.notEqual(transport.status, "failed");
    transport.close();
});

test("local candidate queue is bounded and close suppresses pending delivery", async () => {
    const first = setup();
    const candidate = { candidate: "candidate:early", toJSON: () => ({ candidate: "candidate:early", sdpMid: "0" }) };
    for (let index = 0; index < 63; ++index) first.peer.onicecandidate({ candidate });
    assert.notEqual(first.transport.status, "failed");
    first.peer.onicecandidate({ candidate });
    assert.equal(first.transport.status, "failed");
    await settle();
    assert.equal(first.events.candidates.length, 0);
    const second = setup();
    second.peer.onicecandidate({ candidate });
    second.transport.close(); await settle();
    assert.equal(second.events.candidates.length, 0);
    const third = setup();
    third.peer.onicecandidate({ candidate: { candidate: "x", toJSON: () => ({ candidate: "x".repeat(4097), sdpMid: "0" }) } });
    assert.equal(third.transport.status, "failed");
    const fourth = setup();
    const long = { candidate: "x", toJSON: () => ({ candidate: "x".repeat(4096), sdpMid: "0" }) };
    for (let index = 0; index < 16; ++index) fourth.peer.onicecandidate({ candidate: long });
    assert.equal(fourth.transport.status, "failed");
});

test("closing during local installation suppresses signaling", async () => {
    const { transport, peer, events } = setup();
    let finish;
    peer.setLocalDescription = (value) => new Promise((resolve) => {
        peer.localDescription = { toJSON: () => value }; finish = resolve;
    });
    const operation = transport.startOffer();
    await settle();
    peer.onicecandidate({ candidate: { candidate: "candidate:early", toJSON: () => ({ candidate: "candidate:early", sdpMid: "0" }) } });
    transport.close(); finish(); await operation; await settle();
    assert.deepEqual(events.descriptions, []);
    assert.deepEqual(events.candidates, []);
});

test("relay-only mode waits for a verified local relay and rejects a host pair", async () => {
    const iceServers = [{ urls: "turn:relay.example", username: "user", credential: "password" }];
    const good = setup({ relayPolicy: "requireLocalRelay", iceServers });
    assert.equal(good.peer.config.iceTransportPolicy, "relay");
    await good.transport.startOffer();
    good.peer.report = stats("relay", "host"); good.peer.connect();
    await settle();
    assert.equal(good.transport.status, "ready");
    assert.equal(good.transport.route.current, "turnRelayed");
    good.transport.close();
    assert.equal(good.transport.route.turnHistory, "used");
    const bad = setup({ relayPolicy: "requireLocalRelay", iceServers });
    await bad.transport.startOffer(); bad.peer.connect(); await settle();
    assert.equal(bad.transport.status, "failed");
    assert.throws(() => setup({ relayPolicy: "requireLocalRelay" }), TypeError);
    assert.throws(() => setup({ iceServers: [{ urls: "https://not-an-ice-server" }] }), TypeError);
    assert.throws(() => setup({ iceServers: Array(17).fill({ urls: "stun:example.com" }) }), TypeError);
    assert.throws(() => setup({ iceServers: [{ urls: "turn:example.com", credential: {} }] }), TypeError);
});

test("relay-only mode buffers early binary and suspends when stats disappear", async () => {
    const iceServers = [{ urls: "turn:relay.example" }];
    const { transport, peer, events } = setup({ relayPolicy: "requireLocalRelay", iceServers });
    await transport.startOffer();
    let resolveStats;
    peer.getStats = () => new Promise((resolve) => { resolveStats = resolve; });
    peer.connect();
    peer.channel.onmessage({ data: Uint8Array.of(3).buffer });
    await settle();
    assert.equal(events.bytes.length, 0);
    resolveStats(stats("relay", "host"));
    await settle();
    assert.deepEqual(events.order, ["ready", "binary"]);
    peer.getStats = async () => { throw new Error("statistics unavailable"); };
    await transport.pollRoute(); await settle();
    assert.equal(transport.status, "suspended");
    assert.equal(transport.route.current, "unknown");
    assert.equal(transport.route.turnHistory, "used");
    assert.equal(transport.send(Uint8Array.of(1)), "notReady");
    transport.close();
});

test("overlapping statistics polls share one request and refresh stale results", async () => {
    const { transport, peer } = setup();
    await transport.startOffer();
    const pending = [];
    peer.getStats = () => new Promise((resolve) => pending.push(resolve));
    peer.connect();
    const first = transport.pollRoute(), second = transport.pollRoute();
    assert.equal(pending.length, 1);
    peer.oniceconnectionstatechange();
    pending[0](stats("host", "host"));
    await first; await second; await settle();
    assert.equal(pending.length, 2);
    pending[1](stats("host", "relay"));
    await settle();
    assert.equal(transport.route.current, "turnRelayed");
    transport.close();
});

test("missing selected pair suspends strict relay and stale results cannot reopen it", async () => {
    const { transport, peer } = setup({ relayPolicy: "requireLocalRelay", iceServers: [{ urls: "turn:relay.example" }] });
    await transport.startOffer();
    peer.report = stats("relay", "host"); peer.connect(); await settle();
    assert.equal(transport.status, "ready");
    peer.report = new Map();
    await transport.pollRoute(); await settle();
    assert.equal(transport.status, "suspended");
    assert.equal(transport.send(Uint8Array.of(1)), "notReady");
    let resolveStats;
    peer.getStats = () => new Promise((resolve) => { resolveStats = resolve; });
    const pending = transport.pollRoute();
    transport.close();
    resolveStats(stats("relay", "host"));
    await pending; await settle();
    assert.equal(transport.status, "closed");
    assert.equal(transport.route.current, "unknown");
});

test("relay-only mode buffers inbound data during route revalidation and enforces limits", async () => {
    const options = { relayPolicy: "requireLocalRelay", iceServers: [{ urls: "turn:relay.example" }],
        maxInboundMessages: 1 };
    const first = setup(options);
    await first.transport.startOffer();
    first.peer.report = stats("relay", "host"); first.peer.connect(); await settle();
    first.peer.report = new Map(); await first.transport.pollRoute(); await settle();
    first.peer.channel.onmessage({ data: Uint8Array.of(1).buffer }); await settle();
    assert.equal(first.transport.status, "suspended");
    assert.equal(first.events.bytes.length, 0);
    first.peer.report = stats("relay", "host"); await first.transport.pollRoute(); await settle();
    assert.equal(first.transport.status, "ready");
    assert.deepEqual([...first.events.bytes[0]], [1]);
    first.transport.close();
    const second = setup(options);
    await second.transport.startOffer();
    second.peer.report = stats("relay", "host"); second.peer.connect(); await settle();
    second.peer.report = new Map(); await second.transport.pollRoute(); await settle();
    second.peer.channel.onmessage({ data: Uint8Array.of(1).buffer });
    second.peer.channel.onmessage({ data: Uint8Array.of(2).buffer }); await settle();
    assert.equal(second.transport.status, "failed");
    second.transport.close();
});

test("relay-only readiness retries unavailable statistics and stops retrying on close", async (context) => {
    context.mock.timers.enable({ apis: ["setTimeout"] });
    const { transport, peer, events } = setup({ relayPolicy: "requireLocalRelay", iceServers: [{ urls: "turn:relay.example" }] });
    try {
        await transport.startOffer();
        let calls = 0;
        peer.getStats = async () => {
            ++calls;
            if (calls === 1) throw new Error("temporarily unavailable");
            return calls === 2 ? new Map() : stats("relay", "host");
        };
        peer.connect(); await settle();
        assert.equal(transport.status, "connecting");
        context.mock.timers.tick(250); await settle();
        assert.equal(transport.status, "connecting");
        context.mock.timers.tick(250); await settle();
        assert.equal(calls, 3);
        assert.equal(transport.status, "ready");
        assert.deepEqual(events.states, ["ready"]);
        peer.getStats = async () => { ++calls; return new Map(); };
        await transport.pollRoute();
        assert.equal(transport.status, "suspended");
        transport.close();
        context.mock.timers.tick(250); await settle();
        assert.equal(calls, 4);
    } finally { transport.close(); }
});

test("missing route statistics never claim direct and automatic mode remains usable", async () => {
    const { transport, peer } = setup();
    await transport.startOffer();
    peer.statsError = new Error("statistics unavailable"); peer.connect(); await settle();
    assert.equal(transport.status, "ready");
    assert.equal(transport.route.current, "unknown");
    assert.equal(transport.route.turnHistory, "unknown");
    transport.close();
});

test("input limits and binary-only channel failures are enforced", async () => {
    const { transport, peer } = setup({ maxMessageBytes: 4, maxBufferBytes: 8, maxInboundBytes: 4 });
    await transport.startOffer(); peer.connect(); await settle();
    assert.equal(transport.send(new Uint8Array(5)), "tooLarge");
    peer.channel.bufferedAmount = 7;
    assert.equal(transport.send(new Uint8Array(2)), "backpressure");
    peer.channel.bufferedAmount = 0;
    peer.sctp.maxMessageSize = Infinity;
    assert.equal(transport.send(Uint8Array.of(1)), "accepted");
    peer.sctp = null;
    assert.equal(transport.maxMessageSize(), null);
    assert.equal(transport.send(Uint8Array.of(1)), "notReady");
    peer.channel.onmessage({ data: "text" });
    assert.equal(transport.status, "failed");
    assert.equal(transport.send(Uint8Array.of(1)), "failed");
});

test("backpressure wakes when the browser send buffer falls to a quarter of its limit", async () => {
    const { transport, peer, events } = setup({ maxMessageBytes: 8, maxBufferBytes: 8, maxInboundBytes: 8 });
    await transport.startOffer(); peer.connect(); await settle();
    peer.channel.bufferedAmount = 1;
    assert.equal(transport.send(new Uint8Array(8)), "backpressure");
    assert.equal(peer.channel.bufferedAmountLowThreshold, 2);
    peer.channel.bufferedAmount = 0;
    peer.channel.onbufferedamountlow();
    await settle();
    assert.equal(events.writable, 1);
    assert.equal(transport.send(new Uint8Array(8)), "accepted");
    transport.close();
});

test("queued events are suppressed after close and early binary fails", async () => {
    const first = setup();
    await first.transport.startOffer();
    first.peer.channel.onmessage({ data: Uint8Array.of(1).buffer });
    await settle();
    assert.equal(first.transport.status, "failed");
    assert.equal(first.events.bytes.length, 0);
    const second = setup();
    await second.transport.startOffer(); second.peer.connect();
    second.transport.close(); await settle();
    assert.equal(second.events.states.length, 0);
});

test("candidate and description limits fail before reaching the browser", async () => {
    const { transport } = setup();
    await assert.rejects(transport.addRemoteCandidate({ candidate: "", sdpMid: "0" }), RangeError);
    await assert.rejects(transport.addRemoteCandidate({ candidate: "x".repeat(4097), sdpMid: "0" }), RangeError);
    await assert.rejects(transport.setRemoteDescription({ type: "answer", sdp: "answer" }), TypeError);
    await transport.startOffer();
    await assert.rejects(transport.setRemoteDescription({ type: "answer", sdp: "x".repeat(32769) }), TypeError);
    for (let i = 0; i < 64; ++i) await transport.addRemoteCandidate({ candidate: `candidate:${i}`, sdpMid: "0" });
    await assert.rejects(transport.addRemoteCandidate({ candidate: "candidate:overflow", sdpMid: "0" }), RangeError);
    transport.close();
});

test("candidate data is copied, bounded and end markers are not emitted", async () => {
    const { transport, peer, events } = setup();
    await transport.startOffer();
    const candidate = { candidate: "candidate:original", sdpMid: "0", extra: "x".repeat(100000) };
    await transport.addRemoteCandidate(candidate);
    candidate.candidate = "candidate:changed";
    peer.onicecandidate({ candidate: { candidate: "", toJSON: () => ({ candidate: "" }) } });
    await transport.setRemoteDescription({ type: "answer", sdp: "answer" });
    await settle();
    assert.equal(peer.candidates[0].candidate, "candidate:original");
    assert.equal("extra" in peer.candidates[0], false);
    assert.deepEqual(events.candidates, []);
    await assert.rejects(transport.addRemoteCandidate({ candidate: "candidate:x", sdpMid: "x".repeat(257) }), TypeError);
    transport.close();
});

test("candidates added during a pending browser operation drain in order", async () => {
    const { transport, peer } = setup();
    await transport.startOffer();
    await transport.setRemoteDescription({ type: "answer", sdp: "answer" });
    const pending = [];
    peer.addIceCandidate = (value) => new Promise((resolve) => pending.push({ value, resolve }));
    const first = transport.addRemoteCandidate({ candidate: "candidate:first", sdpMid: "0" });
    const second = transport.addRemoteCandidate({ candidate: "candidate:second", sdpMid: "0" });
    assert.equal(pending.length, 1);
    pending[0].resolve(); await settle();
    assert.equal(pending.length, 2);
    pending[1].resolve(); await first; await second;
    assert.deepEqual(pending.map((item) => item.value.candidate), ["candidate:first", "candidate:second"]);
    transport.close();
});
