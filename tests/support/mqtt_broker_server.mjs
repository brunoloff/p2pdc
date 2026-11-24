// A minimal MQTT 5 broker over WebSocket for tests: retained messages with expiry, single-level
// (+) wildcards and QoS 0 delivery. It parses packets itself rather than with the client's codec.
import { startWebSocketServer } from "./websocket_server.mjs";

function lengthBytes(value) {
    const out = [];
    do { let byte = value % 128; value = Math.floor(value / 128); if (value) byte |= 128; out.push(byte); } while (value);
    return out;
}
const packet = (first, body) => Buffer.from([first, ...lengthBytes(body.length), ...body]);
const str = (text) => { const bytes = Buffer.from(text); return [bytes.length >> 8, bytes.length & 255, ...bytes]; };
const matches = (filter, topic) => {
    const f = filter.split("/"), t = topic.split("/");
    return f.length === t.length && f.every((part, index) => part === "+" || part === t[index]);
};

function readVarint(body, at) {
    let value = 0, shift = 1, byte;
    do { byte = body[at++]; value += (byte & 127) * shift; shift *= 128; } while (byte & 128);
    return [value, at];
}
function readString(body, at) { const length = body.readUInt16BE(at); return [body.subarray(at + 2, at + 2 + length).toString(), at + 2 + length]; }

export async function startMqttBroker() {
    const retained = new Map(); // topic -> { payload, expires }
    const sessions = new Set();
    const broker = { retained, refuseNextPublish: null, refuseNextSubscribe: null, connects: 0, protocolLevels: [] };
    const deliver = (session, topic, payload, isRetained) =>
        session.connection.send(packet(0x30 | (isRetained ? 1 : 0), [...str(topic), 0, ...payload]));
    function onPublish(session, flags, body) {
        let [topic, at] = readString(body, 0);
        const packetId = body.readUInt16BE(at); at += 2;
        const [propertiesLength, start] = readVarint(body, at);
        let expires = Infinity;
        for (let p = start; p < start + propertiesLength;) {
            if (body[p] === 0x02) { expires = Date.now() + body.readUInt32BE(p + 1) * 1000; p += 5; } else throw new Error("unexpected property");
        }
        const payload = body.subarray(start + propertiesLength);
        const reason = broker.refuseNextPublish ?? 0; broker.refuseNextPublish = null;
        session.connection.send(packet(0x40, [packetId >> 8, packetId & 255, reason, 0]));
        if (reason >= 0x80 || (flags & 6) !== 2 || (flags & 1) !== 1) return;
        retained.set(topic, { payload, expires });
        for (const other of sessions) for (const filter of other.filters) if (matches(filter, topic)) deliver(other, topic, payload, false);
    }
    function onSubscribe(session, body) {
        const packetId = body.readUInt16BE(0);
        let [propertiesLength, at] = readVarint(body, 2); at += propertiesLength;
        const [filter, next] = readString(body, at), options = body[next];
        const reason = broker.refuseNextSubscribe ?? 0; broker.refuseNextSubscribe = null;
        session.connection.send(packet(0x90, [packetId >> 8, packetId & 255, 0, reason]));
        if (reason >= 0x80) return;
        session.filters.add(filter);
        if (((options >> 4) & 3) !== 0) return;
        for (const [topic, value] of retained)
            if (value.expires > Date.now() && matches(filter, topic)) deliver(session, topic, value.payload, true);
    }
    function onPacket(session, type, flags, body) {
        if (type === 1) {
            ++broker.connects; broker.protocolLevels.push(body[6]);
            session.connection.send(packet(0x20, [0, 0, 0]));
        } else if (type === 3) onPublish(session, flags, body);
        else if (type === 8) onSubscribe(session, body);
        else if (type === 10) {
            const packetId = body.readUInt16BE(0);
            let [propertiesLength, at] = readVarint(body, 2); at += propertiesLength;
            session.filters.delete(readString(body, at)[0]);
            session.connection.send(packet(0xb0, [packetId >> 8, packetId & 255, 0, 0]));
        } else if (type === 12) session.connection.send(packet(0xd0, []));
    }
    const server = await startWebSocketServer((connection) => {
        const session = { connection, filters: new Set(), buffer: Buffer.alloc(0) };
        sessions.add(session);
        connection.onMessage = (data) => {
            session.buffer = Buffer.concat([session.buffer, Buffer.from(data)]);
            for (;;) {
                if (session.buffer.length < 2) return;
                const [length, at] = readVarint(session.buffer, 1);
                if (session.buffer.length < at + length) return;
                const first = session.buffer[0], body = Buffer.from(session.buffer.subarray(at, at + length));
                session.buffer = session.buffer.subarray(at + length);
                onPacket(session, first >> 4, first & 15, body);
            }
        };
        connection.onClose = () => sessions.delete(session);
    }, ["mqtt"]);
    return Object.assign(broker, { url: server.url("/mqtt"), server, sessions, close: () => server.close() });
}
