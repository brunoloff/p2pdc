import { BrowserConnection } from "./browser_connection.mjs";
import { BrowserLease } from "./browser_lease.mjs";
import { ProxyMailbox } from "./proxy_mailbox.mjs";
import { ProxyOutbox } from "./proxy_outbox.mjs";
import { messageLimit } from "./message_limit.mjs";
import { isTransientProxyError } from "./proxy_codec.mjs";
import { copyBytes, copyIdentity, copyTransport, matchingIdentity, sameEndpoint, unknownRoute, wipeIdentity }
    from "./browser_owner_utils.mjs";

function copyPeer(peer) {
    if (!Number.isSafeInteger(peer?.handle) || peer.handle < 1 || peer.handle > 0xffffffff)
        throw new TypeError("invalid peer handle");
    return { handle: peer.handle, signingKey: copyBytes(peer.signingKey, 32), recipientKey: copyBytes(peer.recipientKey, 32),
        instance: copyBytes(peer.instance, 16), inbox: copyBytes(peer.inbox, 20) };
}

// Single outgoing attempt. A verified advertisement is identity, not consent;
// the application authorizes the supplied peer before constructing this owner.
export class BrowserOutgoing {
    constructor(module, proxy, identity, watch, peer, options, handlers) {
        for (const name of ["onState", "onRoute", "onMessage", "onWritable", "onStatus", "onError"]) {
            if (typeof handlers?.[name] !== "function") throw new TypeError(`missing ${name} callback`);
        }
        if (!watch?.healthy?.() || watch.discovery?.module !== module) throw new TypeError("active watch required");
        this.timeoutMs = options?.timeoutMs ?? 30000;
        if (!Number.isSafeInteger(this.timeoutMs) || this.timeoutMs < 1 || this.timeoutMs > 120000)
            throw new RangeError("invalid connection timeout");
        this.maxMessageBytes = messageLimit(options?.maxMessageBytes);
        this.now = options?.now;
        if (this.now !== undefined && typeof this.now !== "function") throw new TypeError("invalid clock");
        this.transportOptions = copyTransport(options?.transport, "offerer");
        this.peer = copyPeer(peer); this.identity = copyIdentity(identity);
        this.module = module; this.proxy = proxy; this.watch = watch; this.handlers = handlers; this.options = options;
        this.state = "new"; this.epoch = 0; this.connection = null; this.outbox = null; this.authenticated = false;
        this.signalingUnavailable = false;
        this.queue = []; this.bytes = 0; this.connectionStarted = false;
        try { this.initialize(); } catch (error) { this.dispose(); throw error; }
    }
    initialize() {
        this.lease = new BrowserLease(this.module, this.proxy, this.identity,
            { onPublished: () => this.published(), onError: (reason) => this.leaseError(reason) }, this.options.lease);
        const endpoint = this.lease.endpoint();
        if (!endpoint || !matchingIdentity(this.lease, this.watch))
            throw new Error("watch and local identity do not match");
        this.mailbox = new ProxyMailbox(this.proxy, endpoint.inbox,
            { onRecord: (wire) => this.receive(wire), onCatchup: () => {},
                onError: (error) => this.signalFailure("mailbox", error) });
    }
    async start() {
        if (this.state !== "new") throw new Error("outgoing connection cannot be started");
        this.state = "starting";
        if (!await this.mailbox.start() || this.state !== "starting") {
            if (this.state === "starting") this.fail("mailbox");
            return false;
        }
        if (!this.lease.start()) { this.fail("lease"); return false; }
        return this.state !== "failed" && this.state !== "closed";
    }
    published() {
        if (this.state !== "starting") return;
        const peer = this.watch.current(this.peer.handle), local = this.lease.self();
        if (!this.watch.healthy() || !peer || !local || !sameEndpoint(peer, this.peer)) { this.fail("stalePeer"); return; }
        try {
            const attemptId = crypto.getRandomValues(new Uint8Array(16));
            if (!attemptId.some(Boolean)) throw new Error("invalid attempt identifier");
            this.outbox = new ProxyOutbox(this.proxy, peer.inbox,
                { onPublished: () => {}, onError: (error) => this.signalFailure("signaling", error) });
            this.connection = new BrowserConnection(this.module, { secure: { ...this.identity, role: "offerer",
                timeoutMs: this.timeoutMs, maxMessageBytes: this.maxMessageBytes,
                remoteSigningKey: peer.signingKey, remoteBoxKey: peer.recipientKey,
                localInstance: local.instance, remoteInstance: peer.instance, attemptId },
                transport: this.transportOptions, now: this.now }, this.connectionHandlers());
            this.wipe(); this.state = "connecting";
            void this.startConnection();
        } catch { this.fail("connection"); }
    }
    async startConnection() {
        try {
            await this.connection.start();
            if (["failed", "closed"].includes(this.state)) return;
            this.connectionStarted = true;
            while (this.queue.length && !["failed", "closed"].includes(this.state)) this.deliver(this.queue.shift());
            this.bytes = 0;
        } catch { this.fail("connection"); }
    }
    connectionHandlers() {
        return { onSignal: (wire) => {
                if (!this.signalingUnavailable && this.outbox.send(wire) !== "accepted") throw new Error("signaling queue failed");
            },
            onState: (value) => {
                if (value === "failed") return;
                if (value === "ready" && !this.authenticated) { this.authenticated = true; this.lease.close(); }
                this.state = value; this.notify("onState", value);
            },
            onRoute: (value) => this.notify("onRoute", value), onMessage: (value) => this.notify("onMessage", value),
            onWritable: () => this.notify("onWritable"), onError: (reason) => this.fail(reason) };
    }
    leaseError(reason) { if (reason === "retrying") this.notify("onStatus", reason); else this.fail(reason); }
    signalFailure(reason, error) {
        if (["closed", "failed"].includes(this.state)) return;
        if (!this.connection?.authenticated || !isTransientProxyError(error)) { this.fail(reason); return; }
        if (this.signalingUnavailable) return;
        this.signalingUnavailable = true; this.mailbox.close(); this.outbox?.close();
        this.notify("onStatus", "signalingUnavailable");
    }
    receive(wire) {
        if (["closed", "failed"].includes(this.state)) return;
        if (this.connectionStarted) { this.deliver(wire); return; }
        if (this.queue.length >= 64 || wire.length > 524288 - this.bytes) { this.fail("backpressure"); return; }
        this.queue.push(wire.slice()); this.bytes += wire.length;
    }
    deliver(wire) {
        const result = this.connection.receive(wire);
        if (result !== "accepted" && result !== "ignored") this.fail("signaling");
    }
    notify(name, value) {
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (epoch !== this.epoch || ["closed", "failed"].includes(this.state)) return;
            try { this.handlers[name](value); } catch { this.fail("callback"); }
        });
    }
    send(bytes) {
        if (this.state === "closed") return "closed";
        if (this.state === "failed") return "failed";
        return this.connection ? this.connection.send(bytes) : "notReady";
    }
    route() { return this.connection ? this.connection.route() : unknownRoute(); }
    maxMessageSize() { return this.connection?.maxMessageSize() ?? null; }
    pollRoute() { return this.connection ? this.connection.pollRoute() : Promise.resolve(unknownRoute()); }
    wipe() { wipeIdentity(this.identity); this.identity = null; }
    dispose() {
        this.wipe(); this.mailbox?.close(); this.outbox?.close(); this.connection?.close(); this.lease?.close();
        this.queue.length = 0; this.bytes = 0;
    }
    fail(reason) {
        if (["closed", "failed"].includes(this.state)) return;
        this.state = "failed"; ++this.epoch; this.dispose();
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (this.state !== "failed" || epoch !== this.epoch) return;
            try { this.handlers.onState("failed"); } catch (error) { console.error("p2pdc: outgoing state callback failed", error); }
            if (this.state !== "failed" || epoch !== this.epoch) return;
            try { this.handlers.onError(reason); } catch (error) { console.error("p2pdc: outgoing error callback failed", error); }
        });
    }
    close() { if (this.state !== "closed") { this.state = "closed"; ++this.epoch; this.dispose(); } }
}
