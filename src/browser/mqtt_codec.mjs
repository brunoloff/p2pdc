// The few MQTT 5 packets a record backend needs (OASIS MQTT 5.0, section 3).
import { ProxyError } from "./proxy_codec.mjs";

export const MQTT = { CONNECT: 1, CONNACK: 2, PUBLISH: 3, PUBACK: 4, SUBSCRIBE: 8, SUBACK: 9,
                      UNSUBSCRIBE: 10, UNSUBACK: 11, PINGREQ: 12, PINGRESP: 13, DISCONNECT: 14 };
const messageExpiry = 0x02;

function varint(value) {
    const out = [];
    do { let byte = value % 128; value = Math.floor(value / 128); if (value > 0) byte |= 128; out.push(byte); } while (value > 0);
    return out;
}
const u16 = (value) => [value >> 8, value & 255];
const u32 = (value) => [value >>> 24, (value >>> 16) & 255, (value >>> 8) & 255, value & 255];
function utf8(text) { const bytes = new TextEncoder().encode(text); return [...u16(bytes.length), ...bytes]; }
function packet(type, flags, ...parts) {
    const body = parts.flatMap((part) => (part instanceof Uint8Array ? [...part] : part));
    return Uint8Array.from([(type << 4) | flags, ...varint(body.length), ...body]);
}

export function connectPacket(clientId, keepAliveSeconds) {
    return packet(MQTT.CONNECT, 0, utf8("MQTT"), [5, 0x02], u16(keepAliveSeconds), [0], utf8(clientId));
}
// QoS 1, so that the broker acknowledges keeping it; retained, expiring after seconds.
export function publishPacket(topic, packetId, payload, expirySeconds) {
    return packet(MQTT.PUBLISH, 0x03, utf8(topic), u16(packetId), [5, messageExpiry, ...u32(expirySeconds)], payload);
}
// Retain handling 0 replays stored messages on every subscribe, 2 never does; QoS 0.
export function subscribePacket(packetId, filter, replayRetained) {
    return packet(MQTT.SUBSCRIBE, 0x02, u16(packetId), [0], utf8(filter), [replayRetained ? 0x00 : 0x20]);
}
export function unsubscribePacket(packetId, filter) { return packet(MQTT.UNSUBSCRIBE, 0x02, u16(packetId), [0], utf8(filter)); }
export const pingPacket = () => packet(MQTT.PINGREQ, 0);
export const pubackPacket = (packetId) => packet(MQTT.PUBACK, 0, u16(packetId));

class Reader {
    constructor(bytes) { this.bytes = bytes; this.at = 0; }
    byte() { if (this.at >= this.bytes.length) throw new ProxyError("invalid_stream", "short MQTT packet"); return this.bytes[this.at++]; }
    u16() { return (this.byte() << 8) | this.byte(); }
    varint() { let value = 0; for (let shift = 1, byte; ; shift *= 128) { byte = this.byte(); value += (byte & 127) * shift; if (!(byte & 128)) return value; } }
    string() { const length = this.u16(), end = this.at + length; if (end > this.bytes.length) throw new ProxyError("invalid_stream", "short MQTT string"); const text = new TextDecoder().decode(this.bytes.subarray(this.at, end)); this.at = end; return text; }
    skip(length) { this.at += length; }
}

// Splits a byte stream into packets: type, flags, body.
export class PacketDecoder {
    constructor(maxPacketBytes) { this.buffer = new Uint8Array(0); this.max = maxPacketBytes; }
    push(chunk) {
        const joined = new Uint8Array(this.buffer.length + chunk.length);
        joined.set(this.buffer); joined.set(chunk, this.buffer.length);
        this.buffer = joined;
        const packets = [];
        for (;;) {
            if (this.buffer.length < 2) break;
            let length = 0, at = 1;
            for (let shift = 1; ; shift *= 128) {
                if (at >= this.buffer.length) return packets;
                const byte = this.buffer[at++]; length += (byte & 127) * shift;
                if (!(byte & 128)) break;
                if (at > 4) throw new ProxyError("invalid_stream", "bad MQTT length");
            }
            if (length > this.max) throw new ProxyError("invalid_stream", "oversized MQTT packet");
            if (this.buffer.length < at + length) break;
            packets.push({ type: this.buffer[0] >> 4, flags: this.buffer[0] & 15, body: this.buffer.slice(at, at + length) });
            this.buffer = this.buffer.slice(at + length);
        }
        return packets;
    }
}

// Reason codes from CONNACK, SUBACK and PUBACK: the first byte after the fixed fields.
export function parseAck(type, body) {
    const reader = new Reader(body);
    if (type === MQTT.CONNACK) { reader.byte(); return { reason: reader.byte() }; }
    const packetId = reader.u16();
    if (type === MQTT.PUBACK) return { packetId, reason: body.length > 2 ? reader.byte() : 0 };
    reader.skip(reader.varint()); // properties
    return { packetId, reason: reader.byte() };
}
export function parsePublish(flags, body) {
    const reader = new Reader(body), topic = reader.string(), qos = (flags >> 1) & 3;
    const packetId = qos > 0 ? reader.u16() : null;
    reader.skip(reader.varint()); // properties
    return { topic, packetId, retained: (flags & 1) === 1, payload: body.slice(reader.at) };
}

// What an error reason code means for trying again (MQTT 5, 2.4).
export function reasonError(what, reason) {
    const busy = [0x89, 0x97, 0x9f].includes(reason); // server busy, quota exceeded, rate exceeded
    const error = new ProxyError(busy ? "http_error" : "rejected", `MQTT ${what} refused (reason 0x${reason.toString(16)})`);
    if (busy) error.status = reason === 0x89 ? 503 : 429;
    return error;
}
