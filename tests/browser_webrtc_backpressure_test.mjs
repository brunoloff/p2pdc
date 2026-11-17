import test from "node:test";
import assert from "node:assert/strict";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

test("backpressure wakeup leaves room for a maximum-size message", async () => {
    for (const maximum of [7, 8]) {
        let writable = 0;
        const peer = { connectionState: "connected", iceConnectionState: "connected",
            sctp: { maxMessageSize: 100 }, getStats: async () => new Map(), close() {} };
        const handlers = { onDescription() {}, onCandidate() {}, onState() {}, onBinary() {},
            onRoute() {}, onError() {}, onWritable() { ++writable; } };
        const transport = new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "automatic",
            maxMessageBytes: maximum, maxBufferBytes: 8, maxInboundBytes: 8,
            peerConnectionFactory: () => peer }, handlers);
        const channel = { label: "p2pdc/reliable/v1", ordered: true, maxPacketLifeTime: null,
            maxRetransmits: null, readyState: "open", bufferedAmount: 2, send() {}, close() {} };
        transport.attach(channel);
        assert.equal(transport.send(new Uint8Array(maximum)), "backpressure");
        assert.equal(channel.bufferedAmountLowThreshold, 8 - maximum);
        channel.bufferedAmount = channel.bufferedAmountLowThreshold;
        channel.onbufferedamountlow();
        await Promise.resolve();
        assert.equal(writable, 1);
        assert.equal(transport.send(new Uint8Array(maximum)), "accepted");
        transport.close();
    }
});
