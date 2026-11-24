import { ProxyError, decodeBase64, encodeBase64, keyHex } from "./proxy_codec.mjs";
import { publicKey, sign } from "./schnorr.mjs";
import { RecordCollector, randomId, rateGate, recordLifetimeSeconds } from "./store_limits.mjs";
import { PendingReplies, SocketChannel } from "./websocket_channel.mjs";

// Application data (NIP-78); the `x` tag holds the key, which relays index (NIP-01).
const kind = 30078;
const toHex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
const now = () => Math.floor(Date.now() / 1000);

// A relay's refusal (NIP-01 machine-readable prefixes): rate limits and relay errors may
// pass; anything else will not change by asking again.
function refusal(message) {
    const text = typeof message === "string" ? message : "";
    const prefix = text.split(":")[0];
    if (prefix === "rate-limited" || prefix === "error") {
        const error = new ProxyError("http_error", `relay: ${text}`);
        error.status = prefix === "rate-limited" ? 429 : 503;
        return error;
    }
    return new ProxyError("rejected", `relay refused: ${text}`);
}

function tag(event, name) { return event.tags?.find((entry) => Array.isArray(entry) && entry[0] === name)?.[1]; }

// One Nostr relay as a record backend (see docs/DISCOVERY_BACKENDS.md). Values are events
// signed with a throwaway key; whatever the relay returns is untrusted. Stores may share one
// relay client, each passing its own limits; `limits` is the default.
export class NostrRelay {
    constructor(url, limits) {
        this.limits = limits;
        this.streamOrigin = null; // every watch shares the one connection
        this.secret = crypto.getRandomValues(new Uint8Array(32));
        this.pubkey = toHex(publicKey(this.secret));
        this.subscriptions = new Map(); // id -> { event(record), eose(), closed(error) }
        this.replies = new PendingReplies(limits.timeoutMs);
        this.channel = new SocketChannel(url, { message: (data) => this.receive(data), close: (error) => this.lost(error) },
                                         { timeoutMs: limits.timeoutMs });
    }
    async event(key, value) {
        const created = now();
        const tags = [["d", randomId()], ["x", keyHex(key)], ["expiration", String(created + recordLifetimeSeconds)]];
        const content = encodeBase64(value);
        const serial = new TextEncoder().encode(JSON.stringify([0, this.pubkey, created, kind, tags, content]));
        const id = new Uint8Array(await crypto.subtle.digest("SHA-256", serial));
        return { id: toHex(id), pubkey: this.pubkey, created_at: created, kind, tags, content,
                 sig: toHex(await sign(id, this.secret)) };
    }
    async put(key, value, signal, limits = this.limits) {
        if (!(value instanceof Uint8Array) || value.length > limits.maxValueBytes)
            throw new TypeError("invalid or oversized payload");
        const event = await this.event(key, value);
        await this.channel.ready();
        const reply = this.replies.wait(`ok:${event.id}`, signal, limits.timeoutMs);
        this.channel.send(JSON.stringify(["EVENT", event]));
        const [accepted, message] = await reply;
        if (!accepted) throw refusal(message);
    }
    // The record an event carries, or null for events that are not a current value of key.
    record(event, hex, limits) {
        if (!event || event.kind !== kind || tag(event, "x") !== hex || typeof event.id !== "string") return null;
        const expires = Number(tag(event, "expiration"));
        if (Number.isFinite(expires) && expires <= now()) return null;
        try { return { bytes: decodeBase64(event.content, limits.maxValueBytes), expired: false, id: event.id }; }
        catch { return null; }
    }
    async get(key, signal, options = {}, limits = this.limits) {
        const hex = keyHex(key), collector = new RecordCollector(limits, options.deduplicateByContent === true);
        const id = randomId();
        await this.channel.ready();
        const done = this.replies.wait(`eose:${id}`, signal, limits.timeoutMs);
        this.subscriptions.set(id, {
            event: (event) => { const record = this.record(event, hex, limits); if (record) collector.add(record); },
            eose: () => this.replies.settle(`eose:${id}`), closed: (error) => this.replies.fail(`eose:${id}`, error),
        });
        this.channel.send(JSON.stringify(["REQ", id, { kinds: [kind], "#x": [hex], limit: limits.maxValues }]));
        try { await done; } finally { this.unsubscribe(id); }
        return collector.values;
    }
    listen(key, onRecord, signal, limits = this.limits) {
        const hex = keyHex(key), id = randomId(), admit = rateGate(limits.maxRecordsPerSecond);
        let resolveReady, resolveDone, rejectDone;
        const ready = new Promise((resolve) => { resolveReady = resolve; });
        const done = new Promise((resolve, reject) => { resolveDone = resolve; rejectDone = reject; });
        const end = (error) => { this.unsubscribe(id); resolveReady(false); if (error) rejectDone(error); else resolveDone(); };
        this.subscriptions.set(id, {
            event: (event) => { const record = this.record(event, hex, limits); if (record && admit()) onRecord(record); },
            eose: () => resolveReady(true), closed: end,
        });
        signal?.addEventListener("abort", () => end(null), { once: true });
        this.channel.ready().then(() => {
            if (this.subscriptions.has(id))
                this.channel.send(JSON.stringify(["REQ", id, { kinds: [kind], "#x": [hex], since: now() }]));
        }, end);
        return { ready, done, close: () => end(null) };
    }
    unsubscribe(id) {
        if (!this.subscriptions.delete(id)) return;
        try { this.channel.send(JSON.stringify(["CLOSE", id])); } catch { /* the connection is gone already */ }
    }
    receive(data) {
        let message;
        try { message = JSON.parse(typeof data === "string" ? data : new TextDecoder().decode(data)); } catch { return; }
        if (!Array.isArray(message)) return;
        const [type, first, second, third] = message;
        const subscription = typeof first === "string" ? this.subscriptions.get(first) : undefined;
        if (type === "OK" && typeof first === "string") this.replies.settle(`ok:${first}`, [second === true, third]);
        else if (type === "EVENT" && subscription) subscription.event(second);
        else if (type === "EOSE" && subscription) subscription.eose();
        else if (type === "CLOSED" && subscription) { this.subscriptions.delete(first); subscription.closed(refusal(second)); }
    }
    close() {
        this.channel.close();
        this.lost(new ProxyError("network_error", "relay client closed"));
    }
    lost(error) {
        this.replies.failAll(error);
        for (const [id, subscription] of [...this.subscriptions]) { this.subscriptions.delete(id); subscription.closed(error); }
    }
}
