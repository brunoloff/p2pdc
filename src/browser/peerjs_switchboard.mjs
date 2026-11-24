import { ProxyError, decodeBase64, encodeBase64, keyHex } from "./proxy_codec.mjs";
import { KeyHost } from "./peerjs_host.mjs";
import { PeerJsSocket } from "./peerjs_socket.mjs";
import { RecordCollector, randomId, rateGate, recordLifetimeSeconds } from "./store_limits.mjs";
import { PendingReplies } from "./websocket_channel.mjs";

const renewMs = 30000, sweepMs = 30000;
const hostId = (hex) => `p2pdc${hex}`;
class HostGone extends ProxyError { constructor() { super("network_error", "the key's host left"); } }
function full() { const error = new ProxyError("http_error", "the key's host is full"); error.status = 429; return error; }

// A PeerJS server as a record backend (see docs/DISCOVERY_BACKENDS.md). The server only
// forwards messages between connected IDs, so each key has a host: whoever registers the ID
// derived from the key keeps its values and serves the others, and when the host leaves the
// next store to need the key takes over and puts its own values again.
export class PeerJsSwitchboard {
    constructor(url, key, limits) {
        this.url = url; this.key = key; this.limits = limits; this.streamOrigin = null;
        this.hosts = new Map(); // key hex -> { socket, host }
        this.routing = new Map(); // key hex -> route() in progress
        this.remote = new Set(); // keys hosted elsewhere, as far as we know
        this.own = new Map(); // key hex -> Map(id -> { data, expires }): our values, put again on host changes
        this.watches = new Map(); // sub -> { hex, deliver(id, data), subscribe(), end(error), renew }
        this.waiting = new Map(); // rid -> host id, to fail requests when their host leaves
        this.replies = new PendingReplies(limits.timeoutMs);
        this.client = new PeerJsSocket(url, key, `p2pdcc${randomId()}`,
                                       { message: (src, payload) => this.reply(src, payload), gone: (id) => this.gone(id),
                                         close: (error) => this.lostClient(error) }, limits.timeoutMs);
        this.sweeper = setInterval(() => this.sweep(), sweepMs);
    }
    // Hosts the key when its ID is free; otherwise it is hosted elsewhere.
    route(hex) {
        if (this.hosts.has(hex)) return Promise.resolve("local");
        if (this.remote.has(hex)) return Promise.resolve("remote");
        if (!this.routing.has(hex)) this.routing.set(hex, this.claim(hex).finally(() => this.routing.delete(hex)));
        return this.routing.get(hex);
    }
    async claim(hex) {
        const host = new KeyHost();
        const socket = new PeerJsSocket(this.url, this.key, hostId(hex), {
            message: (src, payload) => this.serve(hex, src, payload), gone: (id) => this.hosts.get(hex)?.host.forget(id),
            close: () => this.unhost(hex, socket) }, this.limits.timeoutMs);
        if (await socket.register() === "taken") { this.remote.add(hex); return "remote"; }
        this.hosts.set(hex, { socket, host });
        for (const [id, value] of this.own.get(hex) ?? []) host.put(id, value.data, value.expires - Date.now());
        return "local";
    }
    // Losing a hosting connection ends the watches on it, which subscribe again.
    unhost(hex, socket) {
        if (this.hosts.get(hex)?.socket !== socket) return;
        this.hosts.delete(hex);
        for (const watch of [...this.watches.values()]) if (watch.hex === hex) watch.end(new ProxyError("stream_closed", "stopped hosting the key"));
    }
    // Runs operation against the key's host, here or remote, once more when a remote host left.
    async withHost(hex, local, remote) {
        for (let attempt = 0; ; ++attempt) {
            if (await this.route(hex) === "local") return local(this.hosts.get(hex).host);
            try { return await remote(); } catch (error) {
                if (!(error instanceof HostGone) || attempt > 0) throw error;
                this.remote.delete(hex);
            }
        }
    }
    async ask(hex, message, signal, timeoutMs) {
        if (await this.client.register() !== "open") throw new ProxyError("rejected", "PeerJS client ID is taken");
        const rid = randomId();
        this.client.send(hostId(hex), { p2pdc: 1, ...message, rid, key: hex }); // its reply comes in a later task
        this.waiting.set(rid, hostId(hex));
        try { return await this.replies.wait(rid, signal, timeoutMs); } finally { this.waiting.delete(rid); }
    }
    async put(key, value, signal, limits = this.limits) {
        if (!(value instanceof Uint8Array) || value.length > limits.maxValueBytes)
            throw new TypeError("invalid or oversized payload");
        const hex = keyHex(key), id = randomId(), data = encodeBase64(value);
        const expires = Date.now() + recordLifetimeSeconds * 1000;
        if (!this.own.has(hex)) this.own.set(hex, new Map());
        this.own.get(hex).set(id, { data, expires });
        await this.withHost(hex, () => { if (!this.keep(hex, id, data)) throw full(); },
                            async () => { if ((await this.ask(hex, { op: "put", id, data }, signal, limits.timeoutMs)).op !== "ack") throw full(); });
    }
    async get(key, signal, options = {}, limits = this.limits) {
        const hex = keyHex(key), collector = new RecordCollector(limits, options.deduplicateByContent === true);
        const values = await this.withHost(hex, (host) => host.values(),
                                           async () => (await this.ask(hex, { op: "get" }, signal, limits.timeoutMs)).values);
        for (const value of Array.isArray(values) ? values : []) {
            try { collector.add({ bytes: decodeBase64(value.data, limits.maxValueBytes), expired: false, id: String(value.id) }); }
            catch { /* untrusted junk from the host */ }
        }
        return collector.values;
    }
    listen(key, onRecord, signal, limits = this.limits) {
        const hex = keyHex(key), sub = randomId(), admit = rateGate(limits.maxRecordsPerSecond);
        let resolveReady, resolveDone, rejectDone, ended = false;
        const ready = new Promise((resolve) => { resolveReady = resolve; });
        const done = new Promise((resolve, reject) => { resolveDone = resolve; rejectDone = reject; });
        const deliver = (id, data) => {
            if (ended || !admit()) return;
            try { onRecord({ bytes: decodeBase64(data, limits.maxValueBytes), expired: false, id: String(id) }); } catch { /* junk */ }
        };
        const end = (error) => {
            if (ended) return;
            ended = true; clearInterval(watch.renew); this.watches.delete(sub);
            this.hosts.get(hex)?.host.listeners.delete(deliver);
            if (this.remote.has(hex)) try { this.client.send(hostId(hex), { p2pdc: 1, op: "unwatch", key: hex, sub }); } catch { /* gone */ }
            resolveReady(false);
            if (error) rejectDone(error); else resolveDone();
        };
        const subscribe = () => this.withHost(hex, (host) => { host.listeners.add(deliver); }, async () => {
            if ((await this.ask(hex, { op: "watch", sub }, signal, limits.timeoutMs)).op !== "ack") throw full();
        });
        const watch = { hex, deliver, subscribe, end, renew: setInterval(() => { subscribe().catch(end); }, renewMs) };
        this.watches.set(sub, watch);
        signal?.addEventListener("abort", () => end(null), { once: true });
        subscribe().then(() => resolveReady(!ended), end);
        return { ready, done, close: () => end(null) };
    }
    // Keeps a value on a key this store hosts and tells its watchers; false when the key is full.
    keep(hex, id, data) {
        const entry = this.hosts.get(hex), watchers = entry?.host.put(id, data, recordLifetimeSeconds * 1000);
        for (const watcher of watchers ?? [])
            try { entry.socket.send(watcher.client, { p2pdc: 1, op: "value", sub: watcher.sub, id, data }); }
            catch { /* the hosting connection is gone */ }
        return Boolean(watchers);
    }
    // A request to a key this store hosts.
    serve(hex, src, payload) {
        const entry = this.hosts.get(hex);
        if (!entry || !payload || payload.p2pdc !== 1 || payload.key !== hex || typeof payload.rid !== "string") return;
        const answer = (message) => { try { entry.socket.send(src, { p2pdc: 1, rid: payload.rid, ...message }); } catch { /* gone */ } };
        if (payload.op === "get") answer({ op: "values", values: entry.host.values() });
        else if (payload.op === "watch") answer({ op: entry.host.watch(src, String(payload.sub)) ? "ack" : "full" });
        else if (payload.op === "unwatch") entry.host.unwatch(src, String(payload.sub));
        else if (payload.op === "put" && typeof payload.id === "string" && typeof payload.data === "string" &&
                 payload.data.length <= 4 * Math.ceil(32768 / 3)) {
            answer({ op: this.keep(hex, payload.id, payload.data) ? "ack" : "full" });
        }
    }
    // Replies and pushed values for this store's client ID.
    reply(src, payload) {
        if (!payload || payload.p2pdc !== 1) return;
        if (payload.op === "value") {
            const watch = this.watches.get(String(payload.sub));
            if (watch && src === hostId(watch.hex)) watch.deliver(payload.id, payload.data);
        } else if (typeof payload.rid === "string" && this.waiting.get(payload.rid) === src) this.replies.settle(payload.rid, payload);
    }
    // A host we wrote to is gone: its requests fail, so that they move to a new host, and its
    // watches subscribe again there.
    gone(id) {
        for (const [rid, host] of this.waiting) if (host === id) this.replies.fail(rid, new HostGone());
        const hex = id.slice(5);
        if (id.length !== 45 || !this.remote.delete(hex)) return;
        for (const watch of [...this.watches.values()]) if (watch.hex === hex) watch.subscribe().catch(watch.end);
    }
    // Without the client connection, remote watches hear nothing more: they end, to be watched again.
    lostClient(error) {
        this.replies.failAll(error);
        for (const watch of [...this.watches.values()]) if (!this.hosts.has(watch.hex)) watch.end(error);
    }
    sweep() {
        const now = Date.now();
        for (const values of this.own.values()) for (const [id, value] of values) if (value.expires <= now) values.delete(id);
        for (const [hex, values] of this.own) if (!values.size) this.own.delete(hex);
        for (const [hex, { socket, host }] of this.hosts) if (host.idle && !this.own.has(hex)) { this.hosts.delete(hex); socket.close(); }
    }
    close() {
        clearInterval(this.sweeper);
        for (const { socket } of this.hosts.values()) socket.close();
        this.hosts.clear();
        this.client.close();
        this.replies.failAll(new ProxyError("network_error", "PeerJS client closed"));
    }
}
