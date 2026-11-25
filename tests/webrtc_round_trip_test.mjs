import test from "node:test";
import assert from "node:assert/strict";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";
import { RoundTripPoller, selectedRoundTripMs } from "../src/browser/webrtc_route.mjs";
import { createWebRtcBridge } from "../src/browser/webrtc_bridge.mjs";

function stats(seconds, extra = []) {
    return new Map([
        ["transport", { type: "transport", selectedCandidatePairId: "pair" }],
        ["pair", { type: "candidate-pair", localCandidateId: "local", remoteCandidateId: "remote", currentRoundTripTime: seconds }],
        ["local", { type: "local-candidate", candidateType: "host" }],
        ["remote", { type: "remote-candidate", candidateType: "host" }],
        ...extra,
    ]);
}
const channel = { readyState: "connecting", bufferedAmount: 0, ordered: true, maxPacketLifeTime: null, maxRetransmits: null,
                  label: "p2pdc/reliable/v1", send() {}, close() { this.readyState = "closed"; } };
class FakePeer {
    constructor() { this.connectionState = "new"; this.iceConnectionState = "new"; this.sctp = { maxMessageSize: 65536 }; this.polls = 0; }
    createDataChannel() { this.channel = { ...channel }; return this.channel; }
    async createOffer() { return { type: "offer", sdp: "offer" }; }
    async setLocalDescription(value) { this.localDescription = { toJSON: () => value }; }
    async getStats() { ++this.polls; return this.report; }
    close() { this.connectionState = "closed"; }
    connect() {
        this.connectionState = "connected"; this.iceConnectionState = "connected";
        this.channel.readyState = "open"; this.onconnectionstatechange();
    }
}
async function settle() { for (let i = 0; i < 5; ++i) await Promise.resolve(); }

test("the selected pair's round-trip time is read in milliseconds, never guessed", () => {
    assert.equal(selectedRoundTripMs(stats(0.0185)), 18.5);
    assert.equal(selectedRoundTripMs(stats(undefined)), null, "not measured yet");
    assert.equal(selectedRoundTripMs(stats(-1)), null);
    assert.equal(selectedRoundTripMs(stats(Number.NaN)), null);
    assert.equal(selectedRoundTripMs(stats(0.02, [["second", { type: "transport", selectedCandidatePairId: "other" }]])), null,
                 "two transports select different pairs");
    assert.equal(selectedRoundTripMs({ values() { throw new Error("broken report"); } }), null);
});

test("connected transports report the round-trip time and refresh it every couple of seconds", async (context) => {
    context.mock.timers.enable({ apis: ["setTimeout"] });
    let peer;
    const routes = [];
    const transport = new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "automatic", peerConnectionFactory: () => (peer = new FakePeer()) },
        { onDescription() {}, onCandidate() {}, onState() {}, onBinary() {}, onRoute: (route) => routes.push(route), onError() {}, onWritable() {} });
    await transport.startOffer();
    peer.report = stats(0.031);
    peer.connect(); await settle();
    assert.equal(routes.at(-1).roundTripMs, 31);
    const polls = peer.polls;
    peer.report = stats(0.045);
    context.mock.timers.tick(1999); await settle();
    assert.equal(peer.polls, polls, "no stats before the interval");
    context.mock.timers.tick(1); await settle();
    assert.equal(peer.polls, polls + 1);
    assert.equal(routes.at(-1).roundTripMs, 45);
    transport.close();
    context.mock.timers.tick(10000); await settle();
    assert.equal(peer.polls, polls + 1, "closing stops the polling");
});

test("the poller runs at most one pending poll and can be stopped", (context) => {
    context.mock.timers.enable({ apis: ["setTimeout"] });
    let runs = 0;
    const poller = new RoundTripPoller(() => ++runs);
    poller.schedule(); poller.schedule();
    context.mock.timers.tick(2000);
    assert.equal(runs, 1);
    poller.schedule(); poller.stop();
    context.mock.timers.tick(5000);
    assert.equal(runs, 1);
});

test("the bridge packs a round-trip sample into the route event", () => {
    const events = [], transports = [];
    const bridge = createWebRtcBridge((...event) => { events.push(event); return true; },
                                      (options, handlers) => { const t = { handlers, close() {} }; transports.push(t); return t; });
    const id = bridge.begin("offerer", "automatic", {});
    bridge.open(id);
    const route = (roundTripMs) => {
        transports[0].handlers.onRoute({ current: "direct", turnHistory: "unknown", selectedPair: { local: "host", remote: "host" }, roundTripMs });
        return events.filter((event) => event[1] === 4).at(-1)[2];
    };
    const base = 1 | 8 | (1 << 4) | (1 << 8);
    assert.equal(route(18.4), base | (1 << 12) | (18 << 13));
    assert.equal(route(null), base, "no sample, no flag");
    assert.equal(route(1e9), base | (1 << 12) | (0xffff << 13), "capped at 65535 ms");
    assert.ok(route(1e9) >= 0, "fits an unsigned 32-bit value without the sign bit");
});
