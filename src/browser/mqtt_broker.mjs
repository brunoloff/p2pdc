import { keyHex, ProxyError } from "./proxy_codec.mjs";
// One line per import: the browser bundle (CMakeLists.txt) drops import lines.
import { MQTT, PacketDecoder, connectPacket, parseAck, parsePublish, pingPacket, pubackPacket, publishPacket, reasonError, subscribePacket, unsubscribePacket } from "./mqtt_codec.mjs";
import { RecordCollector, randomId, rateGate, recordLifetimeSeconds } from "./store_limits.mjs";
import { PendingReplies, SocketChannel } from "./websocket_channel.mjs";

const keepAliveSeconds = 60;
// A lookup ends when the broker has sent nothing for this long after acknowledging it.
const quietMs = 400;
const topicOf = (hex) => `p2pdc/v1/${hex}`;

// One MQTT 5 broker over WebSocket as a record backend (see docs/DISCOVERY_BACKENDS.md). Each
// value is a retained, expiring message on a topic of its own under the key. A client has one
// subscription per key, shared by lookups and watches: messages the broker replays from
// storage go to lookups, live ones to watches. Stores may share one broker client, each
// passing its own limits; `limits` is the default.
export class MqttBroker {
    constructor(url, limits) {
        this.limits = limits;
        this.streamOrigin = null; // every watch shares the one connection
        this.keys = new Map(); // key hex -> { users, lookups: Set, watches: Set }
        this.replies = new PendingReplies(limits.timeoutMs);
        this.decoder = null; this.session = null; this.nextId = 1; this.pinger = null;
        this.channel = new SocketChannel(url, { message: (data) => this.receive(data), close: (error) => this.lost(error) },
                                         { protocols: ["mqtt"], timeoutMs: limits.timeoutMs });
    }
    packetId() { this.nextId = this.nextId % 65535 + 1; return this.nextId; }
    // Resolves once connected to the broker, connecting first when needed.
    connected() {
        if (this.session) return this.session;
        this.session = (async () => {
            await this.channel.ready();
            this.decoder = new PacketDecoder(4 * 32768 + 4096); // the largest values limits allow
            const reply = this.replies.wait("connack");
            this.channel.send(connectPacket(`p2pdc${randomId().slice(0, 16)}`, keepAliveSeconds));
            const { reason } = await reply;
            if (reason >= 0x80) throw reasonError("connection", reason);
            this.pinger = setInterval(() => { try { this.channel.send(pingPacket()); } catch { /* reconnects on next use */ } },
                                      keepAliveSeconds * 500);
        })();
        this.session.catch(() => { this.session = null; });
        return this.session;
    }
    async request(packet, kind, signal, timeoutMs) {
        const reply = this.replies.wait(kind, signal, timeoutMs);
        this.channel.send(packet);
        return reply;
    }
    async put(key, value, signal, limits = this.limits) {
        if (!(value instanceof Uint8Array) || value.length > limits.maxValueBytes)
            throw new TypeError("invalid or oversized payload");
        const topic = `${topicOf(keyHex(key))}/${randomId()}`;
        await this.connected();
        const id = this.packetId();
        const { reason } = await this.request(publishPacket(topic, id, value, recordLifetimeSeconds), `puback:${id}`,
                                              signal, limits.timeoutMs);
        if (reason >= 0x80) throw reasonError("publish", reason);
    }
    // Adds target to the key's lookups or watches, then subscribes when needed: again for a
    // lookup, so that the broker replays stored values, which may arrive with its acknowledgment.
    async join(hex, target, kind, signal, timeoutMs) {
        await this.connected();
        const entry = this.keys.get(hex) ?? { users: 0, lookups: new Set(), watches: new Set() };
        this.keys.set(hex, entry);
        ++entry.users;
        entry[kind].add(target);
        if (entry.users > 1 && kind === "watches") return entry;
        try {
            const id = this.packetId(), replay = kind === "lookups";
            const { reason } = await this.request(subscribePacket(id, `${topicOf(hex)}/+`, replay), `suback:${id}`,
                                                  signal, timeoutMs);
            if (reason >= 0x80) throw reasonError("subscription", reason);
        } catch (error) { this.leave(hex, entry, target); throw error; }
        return entry;
    }
    leave(hex, entry, target) {
        entry.lookups.delete(target); entry.watches.delete(target);
        if (--entry.users > 0 || this.keys.get(hex) !== entry) return;
        this.keys.delete(hex);
        try { this.channel.send(unsubscribePacket(this.packetId(), `${topicOf(hex)}/+`)); } catch { /* gone already */ }
    }
    async get(key, signal, options = {}, limits = this.limits) {
        const hex = keyHex(key), collector = new RecordCollector(limits, options.deduplicateByContent === true);
        let timer = null, finish;
        const quiet = new Promise((resolve) => { finish = resolve; });
        const lookup = (record) => {
            if (record.bytes.length <= limits.maxValueBytes) collector.add(record);
            clearTimeout(timer); timer = setTimeout(finish, quietMs);
        };
        const entry = await this.join(hex, lookup, "lookups", signal, limits.timeoutMs);
        // Rejects on abort, timeout or a lost connection; settled when the lookup ends.
        const waitId = `lookup:${randomId()}`, lost = this.replies.wait(waitId, signal, limits.timeoutMs);
        try {
            if (timer === null) timer = setTimeout(finish, quietMs);
            await Promise.race([quiet, lost]);
        } finally { clearTimeout(timer); this.replies.settle(waitId); this.leave(hex, entry, lookup); }
        return collector.values;
    }
    listen(key, onRecord, signal, limits = this.limits) {
        const hex = keyHex(key), admit = rateGate(limits.maxRecordsPerSecond);
        let resolveReady, resolveDone, rejectDone, entry = null, ended = false;
        const ready = new Promise((resolve) => { resolveReady = resolve; });
        const done = new Promise((resolve, reject) => { resolveDone = resolve; rejectDone = reject; });
        const watch = { record: (record) => { if (!ended && record.bytes.length <= limits.maxValueBytes && admit()) onRecord(record); },
                        lost: (error) => end(error) };
        const end = (error) => {
            if (ended) return;
            ended = true;
            if (entry) this.leave(hex, entry, watch);
            resolveReady(false);
            if (error) rejectDone(error); else resolveDone();
        };
        signal?.addEventListener("abort", () => end(null), { once: true });
        this.join(hex, watch, "watches", signal, limits.timeoutMs).then((joined) => {
            if (ended) { this.leave(hex, joined, watch); return; }
            entry = joined; resolveReady(true);
        }, end);
        return { ready, done, close: () => end(null) };
    }
    receive(data) {
        let packets;
        try { packets = this.decoder.push(new Uint8Array(data)); } catch (error) { this.channel.close(); this.lost(error); return; }
        for (const { type, flags, body } of packets) {
            if (type === MQTT.PUBLISH) this.published(flags, body);
            else if (type === MQTT.CONNACK) this.replies.settle("connack", parseAck(type, body));
            else if (type === MQTT.PUBACK || type === MQTT.SUBACK) {
                const ack = parseAck(type, body);
                this.replies.settle(`${type === MQTT.PUBACK ? "puback" : "suback"}:${ack.packetId}`, ack);
            }
        }
    }
    published(flags, body) {
        const message = parsePublish(flags, body);
        if (message.packetId !== null) this.channel.send(pubackPacket(message.packetId));
        const [, , hex, id] = message.topic.split("/");
        const entry = this.keys.get(hex);
        if (!entry || !id || message.payload.length === 0) return;
        const record = { bytes: message.payload, expired: false, id };
        for (const target of message.retained ? entry.lookups : [...entry.lookups, ...entry.watches])
            typeof target === "function" ? target(record) : target.record(record);
    }
    close() {
        this.channel.close();
        this.lost(new ProxyError("network_error", "broker client closed"));
    }
    lost(error) {
        clearInterval(this.pinger); this.pinger = null; this.session = null;
        this.replies.failAll(error);
        for (const entry of this.keys.values()) for (const watch of [...entry.watches]) watch.lost(error);
        this.keys.clear();
    }
}
