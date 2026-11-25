import test from "node:test";
import assert from "node:assert/strict";
import { DataLink } from "../src/browser/peerjs_link.mjs";

// A channel that records what is sent, and a connection that records closing.
function pair() {
    const channel = { readyState: "open", bufferedAmount: 0, sent: [], send(data) { this.sent.push(data); }, close() { this.readyState = "closed"; } };
    const connection = { connectionState: "connected", closed: false, close() { this.closed = true; } };
    const link = new DataLink("peer", connection, channel), received = [];
    let closes = 0;
    link.onmessage = (message) => received.push(message);
    link.onclose = () => ++closes;
    return { link, channel, connection, received, closes: () => closes };
}

test("long messages go as fragments and arrive whole", () => {
    const sender = pair(), receiver = pair();
    const message = { op: "values", text: "x".repeat(50000) };
    sender.link.send(message);
    assert.equal(sender.channel.sent.length, 4);
    assert.deepEqual(sender.channel.sent.map((part) => part[0]), ["+", "+", "+", "="]);
    for (const part of sender.channel.sent) receiver.channel.onmessage({ data: part });
    assert.deepEqual(receiver.received, [message]);
    sender.link.send({ op: "ack" });
    assert.equal(sender.channel.sent.at(-1), '={"op":"ack"}');
});

test("malformed or oversized input closes the connection", () => {
    for (const data of ["no marker", "={not json", new ArrayBuffer(4), "=[1,2]"]) {
        const { link, channel, connection, received, closes } = pair();
        channel.onmessage({ data });
        assert.equal(received.length, 0, String(data));
        if (data === "=[1,2]") { assert.equal(closes(), 0, "a JSON array is ignored"); continue; }
        assert.equal(closes(), 1, String(data));
        assert.ok(link.closed && connection.closed);
    }
    const { channel, closes } = pair();
    for (let i = 0; i < 200 && !closes(); ++i) channel.onmessage({ data: "+" + "y".repeat(16384) });
    assert.equal(closes(), 1, "a message past the cap closes the connection");
});

test("a closed or failed connection refuses to send and reports once", () => {
    const { link, connection, closes } = pair();
    connection.connectionState = "failed";
    connection.onconnectionstatechange();
    assert.equal(closes(), 1);
    assert.throws(() => link.send({ op: "get" }), { code: "network_error" });
    link.close();
    assert.equal(closes(), 1);
});
