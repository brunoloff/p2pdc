import { ProxyError, decodeBase64, encodeBase64, keyHex } from "./proxy_codec.mjs";
import { KeyHost } from "./peerjs_host.mjs";
import { PeerGone, PeerJsEndpoint, peerJsIceServers } from "./peerjs_endpoint.mjs";
import { RecordCollector, randomId, rateGate, recordLifetimeSeconds } from "./store_limits.mjs";
import { PendingReplies } from "./websocket_channel.mjs";

const renewMs = 30000, sweepMs = 30000, idleHostLinkMs = 60000, maxHostLinks = 128;
const hostId = (hex) => `p2pdc${hex}`;
function full() { const error = new ProxyError("http_error", "the key's host is full"); error.status = 429; return error; }
function untilAborted(promise, signal) {
    if (!signal) return promise;
    return new Promise((resolve, reject) => {
        const abort = () => reject(new DOMException("request aborted", "AbortError"));
        if (signal.aborted) { abort(); return; }
        signal.addEventListener("abort", abort, { once: true });
        promise.then(resolve, reject).finally(() => signal.removeEventListener("abort", abort));
    });
}

// A PeerJS server as a record backend (see docs/DISCOVERY_BACKENDS.md). The server only relays
// WebRTC signalling, so each key has a host: whoever registers the ID derived from the key
// keeps its values and serves the others over WebRTC data connections. When the host leaves,
// the next store to need the key takes over and every store puts its own values again.
export class PeerJsSwitchboard {
    constructor(url, key, limits, rtc = {}) {
        this.url = url; this.key = key; this.limits = limits; this.streamOrigin = null; this.closed = false;
        this.rtc = { RTCPeerConnection: rtc.RTCPeerConnection ?? globalThis.RTCPeerConnection,
                     iceServers: rtc.iceServers ?? peerJsIceServers };
        this.hosts = new Map(); // key hex -> { hex, endpoint, host, links: Map(peer id -> DataLink) }
        this.routing = new Map(); // key hex -> route() in progress
        this.remote = new Set(); // keys hosted elsewhere, as far as we know
        this.links = new Map(); // key hex -> { promise, link, used }: our connection to its remote host
        this.own = new Map(); // key hex -> Map(id -> { data, expires }): our values, put again on host changes
        this.watches = new Map(); // sub -> { hex, deliver(id, data), subscribe(), end(error), renew }
        this.waiting = new Map(); // rid -> key hex, to fail requests when their host leaves
        this.replies = new PendingReplies(limits.timeoutMs);
        this.client = this.endpoint(`p2pdcc${randomId()}`, null, null);
        this.sweeper = setInterval(() => this.sweep(), sweepMs);
    }
    endpoint(id, incoming, close) {
        return new PeerJsEndpoint(this.url, this.key, id, { incoming, close },
                                  { timeoutMs: this.limits.timeoutMs, rtc: this.rtc, maxLinks: maxHostLinks });
    }
    // Hosts the key when its ID is free; otherwise it is hosted elsewhere.
    route(hex) {
        if (typeof this.rtc.RTCPeerConnection !== "function")
            return Promise.reject(new ProxyError("rejected", "PeerJS needs WebRTC data channels, which this runtime lacks"));
        if (this.hosts.has(hex)) return Promise.resolve("local");
        if (this.remote.has(hex)) return Promise.resolve("remote");
        if (!this.routing.has(hex)) this.routing.set(hex, this.claim(hex).finally(() => this.routing.delete(hex)));
        return this.routing.get(hex);
    }
    async claim(hex) {
        const entry = { hex, host: new KeyHost(), links: new Map(), endpoint: null };
        entry.endpoint = this.endpoint(hostId(hex), (link) => this.admit(hex, entry, link), () => this.unhost(hex, entry));
        let state;
        try { state = await entry.endpoint.register(); } catch (error) { entry.endpoint.close(); throw error; }
        if (state === "taken" || this.closed) { entry.endpoint.close(); this.remote.add(hex); return "remote"; }
        this.hosts.set(hex, entry);
        for (const [id, value] of this.own.get(hex) ?? []) entry.host.put(id, value.data, value.expires - Date.now());
        return "local";
    }
    // Losing the hosting ID ends the watches on it, which subscribe again.
    unhost(hex, entry) {
        if (this.hosts.get(hex) !== entry) return;
        this.hosts.delete(hex);
        entry.endpoint.close();
        for (const watch of [...this.watches.values()]) if (watch.hex === hex) watch.end(new ProxyError("stream_closed", "stopped hosting the key"));
    }
    // A data connection to a key this store hosts; one per peer.
    admit(hex, entry, link) {
        if (this.hosts.get(hex) !== entry) { link.close(); return; }
        entry.links.get(link.peer)?.close();
        entry.links.set(link.peer, link);
        link.onmessage = (message) => this.serve(entry, link, message);
        link.onclose = () => {
            if (entry.links.get(link.peer) !== link) return;
            entry.links.delete(link.peer);
            entry.host.forget(link.peer);
        };
    }
    // Runs operation against the key's host, here or remote, once more when a remote host left.
    async withHost(hex, local, remote) {
        for (let attempt = 0; ; ++attempt) {
            if (await this.route(hex) === "local") return local(this.hosts.get(hex).host);
            try { return await remote(); } catch (error) {
                if (!(error instanceof PeerGone) || attempt > 0) throw error;
                this.remote.delete(hex);
            }
        }
    }
    // Our data connection to the key's remote host, opened when first needed.
    hostLink(hex) {
        const current = this.links.get(hex);
        if (current) { current.used = Date.now(); return current.promise; }
        const entry = { used: Date.now(), link: null, promise: null };
        entry.promise = this.client.connect(hostId(hex)).then((link) => this.attach(hex, entry, link));
        entry.promise.catch(() => { if (this.links.get(hex) === entry) this.links.delete(hex); });
        this.links.set(hex, entry);
        return entry.promise;
    }
    attach(hex, entry, link) {
        if (this.links.get(hex) !== entry) { link.close(); throw new PeerGone("the connection to the key's host was dropped"); }
        entry.link = link;
        link.onmessage = (message) => this.reply(hex, message);
        link.onclose = () => this.lostHost(hex, entry);
        return link;
    }
    // The remote host went away: its requests fail, so that they move to a new host, and its
    // watches subscribe again there.
    lostHost(hex, entry) {
        if (this.links.get(hex) !== entry) return;
        this.links.delete(hex);
        for (const [rid, waitingHex] of this.waiting) if (waitingHex === hex) this.replies.fail(rid, new PeerGone("the key's host left"));
        if (!this.remote.delete(hex) || this.closed) return;
        this.reput(hex);
        for (const watch of [...this.watches.values()]) if (watch.hex === hex) watch.subscribe().catch(watch.end);
    }
    // Our values on a key whose host left go to its new host, which may be this store.
    reput(hex) {
        const values = [...(this.own.get(hex) ?? [])];
        if (!values.length) return;
        this.withHost(hex, () => {}, async () => {
            for (const [id, value] of values) await this.ask(hex, { op: "put", id, data: value.data }, null, this.limits.timeoutMs);
        }).catch((error) => console.warn("p2pdc: could not put values again on the key's new PeerJS host", error));
    }
    async ask(hex, message, signal, timeoutMs) {
        const link = await untilAborted(this.hostLink(hex), signal);
        const rid = randomId();
        this.waiting.set(rid, hex);
        const reply = this.replies.wait(rid, signal, timeoutMs);
        try { link.send({ p2pdc: 1, ...message, rid }); } catch (error) { this.replies.fail(rid, error); }
        try { return await reply; } finally { this.waiting.delete(rid); }
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
            if (this.remote.has(hex)) try { this.links.get(hex)?.link?.send({ p2pdc: 1, op: "unwatch", sub }); } catch { /* gone */ }
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
            try { entry.links.get(watcher.client)?.send({ p2pdc: 1, op: "value", sub: watcher.sub, id, data }); }
            catch { /* that watcher's connection is gone */ }
        return Boolean(watchers);
    }
    // A request over a data connection to a key this store hosts.
    serve(entry, link, payload) {
        if (payload.p2pdc !== 1 || this.hosts.get(entry.hex) !== entry) return;
        if (payload.op === "unwatch") { entry.host.unwatch(link.peer, String(payload.sub)); return; }
        if (typeof payload.rid !== "string") return;
        const answer = (message) => { try { link.send({ p2pdc: 1, rid: payload.rid, ...message }); } catch { /* gone */ } };
        if (payload.op === "get") answer({ op: "values", values: entry.host.values() });
        else if (payload.op === "watch") answer({ op: entry.host.watch(link.peer, String(payload.sub)) ? "ack" : "full" });
        else if (payload.op === "put" && typeof payload.id === "string" && typeof payload.data === "string" &&
                 payload.data.length <= 4 * Math.ceil(32768 / 3)) {
            answer({ op: this.keep(entry.hex, payload.id, payload.data) ? "ack" : "full" });
        }
    }
    // Replies and pushed values from the remote host of hex.
    reply(hex, payload) {
        if (payload.p2pdc !== 1) return;
        if (payload.op === "value") {
            const watch = this.watches.get(String(payload.sub));
            if (watch?.hex === hex) watch.deliver(payload.id, payload.data);
        } else if (typeof payload.rid === "string" && this.waiting.get(payload.rid) === hex) this.replies.settle(payload.rid, payload);
    }
    sweep() {
        const now = Date.now();
        for (const values of this.own.values()) for (const [id, value] of values) if (value.expires <= now) values.delete(id);
        for (const [hex, values] of this.own) if (!values.size) this.own.delete(hex);
        for (const [hex, entry] of this.hosts) if (entry.host.idle && !this.own.has(hex)) { this.hosts.delete(hex); entry.endpoint.close(); }
        const busy = new Set([...[...this.watches.values()].map((watch) => watch.hex), ...this.waiting.values()]);
        for (const [hex, entry] of this.links)
            if (entry.link && !busy.has(hex) && now - entry.used > idleHostLinkMs) { this.links.delete(hex); entry.link.close(); }
    }
    close() {
        this.closed = true;
        clearInterval(this.sweeper);
        for (const watch of [...this.watches.values()]) watch.end(null);
        for (const entry of this.hosts.values()) entry.endpoint.close();
        this.hosts.clear();
        this.client.close();
        this.replies.failAll(new ProxyError("network_error", "PeerJS client closed"));
    }
}
