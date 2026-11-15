import test from "node:test";
import assert from "node:assert/strict";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

function setup() {
    const order = [], bytes = [], errors = [];
    const channel = { label: "p2pdc/reliable/v1", ordered: true, maxPacketLifeTime: null,
        maxRetransmits: null, readyState: "connecting", close() { this.readyState = "closed"; } };
    const peer = { connectionState: "connecting", iceConnectionState: "connected", sctp: {},
        createDataChannel: () => channel, close() {},
        createOffer: async () => ({ type: "offer", sdp: "offer" }),
        setLocalDescription: async function (value) { this.localDescription = { toJSON: () => value }; },
        getStats: async () => new Map() };
    const handlers = { onDescription() {}, onCandidate() {}, onRoute() {},
        onError: (reason, category) => { errors.push([reason, category]); order.push("error"); }, onWritable() {},
        onState: (state) => order.push(state), onBinary: (value) => { order.push("binary"); bytes.push(value); } };
    const transport = new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "automatic",
        maxInboundMessages: 1, peerConnectionFactory: () => peer }, handlers);
    return { transport, peer, channel, order, bytes, errors };
}
async function settle() { for (let i = 0; i < 5; ++i) await Promise.resolve(); }

test("open channel buffers messages until connection readiness", async () => {
    const { transport, peer, channel, order, bytes } = setup();
    await transport.startOffer();
    channel.readyState = "open";
    channel.onmessage({ data: Uint8Array.of(7).buffer });
    await settle();
    assert.equal(transport.status, "connecting");
    assert.deepEqual(bytes, []);
    peer.connectionState = "connected";
    peer.onconnectionstatechange(); await settle();
    assert.deepEqual(order, ["ready", "binary"]);
    assert.deepEqual([...bytes[0]], [7]);
    transport.close();
});

test("early messages cannot exceed the inbound limit", async () => {
    const { transport, channel, bytes, order, errors } = setup();
    await transport.startOffer();
    channel.readyState = "open";
    channel.onmessage({ data: Uint8Array.of(1).buffer });
    channel.onmessage({ data: Uint8Array.of(2).buffer });
    await settle();
    assert.equal(transport.status, "failed");
    assert.deepEqual(bytes, []);
    assert.deepEqual(errors, [["receive limit exceeded", "resource"]]);
    assert.deepEqual(order, ["error", "failed"]);
});

test("ordinary channel errors retain the transport category", async () => {
    const { transport, channel, order, errors } = setup();
    await transport.startOffer();
    channel.onerror();
    await settle();
    assert.deepEqual(errors, [["data channel error", "transport"]]);
    assert.deepEqual(order, ["error", "failed"]);
});
