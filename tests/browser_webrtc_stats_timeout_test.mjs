import test from "node:test";
import assert from "node:assert/strict";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

function report(local = "relay") {
    return new Map([
        ["transport", { type: "transport", selectedCandidatePairId: "pair" }],
        ["pair", { type: "candidate-pair", localCandidateId: "local", remoteCandidateId: "remote" }],
        ["local", { type: "local-candidate", candidateType: local }],
        ["remote", { type: "remote-candidate", candidateType: "host" }],
    ]);
}
function setup(relayPolicy = "requireLocalRelay") {
    const states = [], routes = [];
    const channel = { label: "p2pdc/reliable/v1", ordered: true, maxPacketLifeTime: null,
        maxRetransmits: null, readyState: "connecting", close() { this.readyState = "closed"; } };
    const peer = { connectionState: "new", iceConnectionState: "new", sctp: {},
        createDataChannel: () => channel, close() {}, getStats: async () => report(),
        createOffer: async () => ({ type: "offer", sdp: "offer" }),
        setLocalDescription: async function (value) { this.localDescription = { toJSON: () => value }; } };
    const handlers = { onDescription() {}, onCandidate() {}, onBinary() {}, onError() {}, onWritable() {},
        onState: (state) => states.push(state), onRoute: (route) => routes.push(route) };
    const transport = new BrowserWebRtcTransport({ role: "offerer", relayPolicy,
        iceServers: [{ urls: "turn:relay.example" }], peerConnectionFactory: () => peer }, handlers);
    return { transport, peer, channel, states, routes };
}
async function settle() { for (let i = 0; i < 8; ++i) await Promise.resolve(); }
async function connect(fixture) {
    await fixture.transport.startOffer();
    fixture.channel.readyState = "open";
    fixture.peer.connectionState = fixture.peer.iceConnectionState = "connected";
    fixture.peer.onconnectionstatechange(); await settle();
}

test("stalled statistics invalidate a required relay and a late result cannot restore it", async (context) => {
    context.mock.timers.enable({ apis: ["setTimeout"] });
    const f = setup();
    try {
        await connect(f);
        assert.equal(f.transport.status, "ready");
        let resolveOld, resolveFresh, calls = 0;
        f.peer.getStats = () => new Promise((resolve) => {
            ++calls;
            if (calls === 1) resolveOld = resolve; else resolveFresh = resolve;
        });
        const pending = f.transport.pollRoute();
        context.mock.timers.tick(2000); await settle();
        assert.equal((await pending).current, "unknown");
        assert.equal(f.transport.route.turnHistory, "used");
        assert.equal(f.transport.status, "suspended");
        assert.equal(f.transport.send(Uint8Array.of(1)), "notReady");
        await f.transport.pollRoute();
        assert.equal(calls, 1);
        resolveOld(report()); await settle();
        assert.equal(calls, 2);
        assert.equal(f.transport.status, "suspended");
        resolveFresh(report()); await settle();
        assert.equal(f.transport.status, "ready");
        assert.equal(f.transport.route.current, "turnRelayed");
    } finally { f.transport.close(); }
});

test("automatic mode keeps working with unknown route and close suppresses late results", async (context) => {
    context.mock.timers.enable({ apis: ["setTimeout"] });
    const f = setup("automatic");
    try {
        await connect(f);
        let resolveOld;
        f.peer.getStats = () => new Promise((resolve) => { resolveOld = resolve; });
        const pending = f.transport.pollRoute();
        context.mock.timers.tick(2000); await pending; await settle();
        assert.equal(f.transport.route.current, "unknown");
        assert.equal(f.transport.status, "ready");
        const count = f.routes.length;
        f.transport.close(); resolveOld(report()); await settle();
        assert.equal(f.transport.status, "closed");
        assert.equal(f.routes.length, count);
    } finally { f.transport.close(); }
});

test("closing a pending observation settles the caller and ignores a late result", async (context) => {
    context.mock.timers.enable({ apis: ["setTimeout"] });
    const f = setup();
    await connect(f);
    let resolveOld;
    f.peer.getStats = () => new Promise((resolve) => { resolveOld = resolve; });
    const pending = f.transport.pollRoute();
    const count = f.routes.length;
    f.transport.close();
    assert.equal((await pending).current, "unknown");
    resolveOld(report()); await settle();
    context.mock.timers.tick(2000); await settle();
    assert.equal(f.transport.status, "closed");
    assert.equal(f.routes.length, count);
});
