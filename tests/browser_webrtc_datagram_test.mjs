import test from "node:test";
import assert from "node:assert/strict";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

class FakeChannel {
    constructor(label, options = {}) {
        this.label = label; this.ordered = options.ordered ?? true;
        this.maxPacketLifeTime = null; this.maxRetransmits = options.maxRetransmits ?? null;
        this.readyState = "connecting"; this.bufferedAmount = 0; this.sent = [];
    }
    send(value) { this.sent.push(value); }
    close() { this.readyState = "closed"; }
}
class FakePeer {
    constructor() {
        this.connectionState = "new"; this.iceConnectionState = "new";
        this.sctp = { maxMessageSize: 65536 }; this.channels = [];
    }
    createDataChannel(name, options) {
        const channel = new FakeChannel(name, options);
        this.channels.push(channel);
        return channel;
    }
    async createOffer() { return { type: "offer", sdp: "offer" }; }
    async setLocalDescription(value) { this.localDescription = { toJSON: () => value }; }
    async getStats() { return new Map(); }
    close() { this.connectionState = "closed"; }
    connect() {
        this.connectionState = "connected"; this.iceConnectionState = "connected";
        for (const channel of this.channels) channel.readyState = "open";
        this.onconnectionstatechange();
    }
}
async function settle() { for (let i = 0; i < 5; ++i) await Promise.resolve(); }
function setup(role, withHandler = true) {
    const received = [], errors = [];
    let peer;
    const handlers = { onDescription() {}, onCandidate() {}, onState() {}, onBinary() {}, onRoute() {},
                       onError: (reason) => errors.push(reason), onWritable() {} };
    if (withHandler) handlers.onDatagram = (bytes) => received.push(bytes);
    const transport = new BrowserWebRtcTransport({ role, relayPolicy: "automatic",
        peerConnectionFactory: () => { peer = new FakePeer(); return peer; } }, handlers);
    return { transport, peer, received, errors };
}
async function readyOfferer(withHandler = true) {
    const side = setup("offerer", withHandler);
    await side.transport.startOffer();
    return side;
}
const datagramChannel = (peer) => peer.channels.find((channel) => channel.label === "p2pdc/datagram/v1");

test("the offerer opens an unordered channel without retransmission", async () => {
    const { transport, peer, received } = await readyOfferer();
    assert.equal(datagramChannel(peer), undefined);
    transport.enableDatagrams();
    const channel = datagramChannel(peer);
    assert.equal(channel.ordered, false);
    assert.equal(channel.maxRetransmits, 0);
    assert.equal(transport.sendDatagram(new Uint8Array([1])), "notReady");
    peer.connect();
    await settle();
    assert.equal(transport.datagramsAvailable(), true);
    assert.equal(transport.sendDatagram(new Uint8Array([1, 2])), "accepted");
    assert.deepEqual([...channel.sent[0]], [1, 2]);
    channel.onmessage({ data: new Uint8Array([3]).buffer });
    assert.deepEqual(received.map((bytes) => [...bytes]), [[3]]);
    transport.close();
});

test("oversized or backed-up datagrams are refused without failing", async () => {
    const { transport, peer, received, errors } = await readyOfferer();
    transport.enableDatagrams();
    peer.connect();
    await settle();
    const channel = datagramChannel(peer);
    assert.equal(transport.sendDatagram(new Uint8Array(1025)), "tooLarge");
    channel.bufferedAmount = 65537;
    assert.equal(transport.sendDatagram(new Uint8Array(1)), "backpressure");
    channel.onmessage({ data: new ArrayBuffer(1025) });
    channel.onmessage({ data: "text" });
    assert.deepEqual(received, []);
    assert.deepEqual(errors, []);
    transport.close();
});

test("the answerer accepts only a matching channel after enabling datagrams", () => {
    const early = setup("answerer");
    early.peer.ondatachannel({ channel: new FakeChannel("p2pdc/datagram/v1", { ordered: false, maxRetransmits: 0 }) });
    assert.equal(early.transport.status, "failed");

    const reliable = setup("answerer");
    reliable.transport.enableDatagrams();
    reliable.peer.ondatachannel({ channel: new FakeChannel("p2pdc/datagram/v1") });
    assert.equal(reliable.transport.status, "failed");

    const good = setup("answerer");
    good.transport.enableDatagrams();
    good.peer.ondatachannel({ channel: new FakeChannel("p2pdc/datagram/v1", { ordered: false, maxRetransmits: 0 }) });
    assert.notEqual(good.transport.status, "failed");
    good.transport.close();
});

test("without a handler datagrams stay off", async () => {
    const { transport, peer } = await readyOfferer(false);
    transport.enableDatagrams();
    assert.equal(datagramChannel(peer), undefined);
    assert.equal(transport.datagramsAvailable(), false);
    transport.close();
});
