import { BrowserConnection } from "./browser_connection.mjs";
import { BrowserListener } from "./browser_listener.mjs";
import { ProxyOutbox } from "./proxy_outbox.mjs";
import { messageLimit } from "./message_limit.mjs";
import { isTransientProxyError } from "./proxy_codec.mjs";
import { copyIdentity, copyTransport, matchingIdentity, unknownRoute, wipeIdentity } from "./browser_owner_utils.mjs";

// Single incoming attempt. Verified requests still require explicit acceptance.
export class BrowserIncoming {
    constructor(module, proxy, identity, watch, options, handlers) {
        for (const name of ["onPublished", "onRequest", "onState", "onRoute", "onMessage", "onWritable", "onStatus", "onError"]) {
            if (typeof handlers?.[name] !== "function") throw new TypeError(`missing ${name} callback`);
        }
        if (!watch?.healthy?.() || watch.discovery?.module !== module) throw new TypeError("active watch required");
        this.timeoutMs = options?.timeoutMs ?? 30000;
        if (!Number.isSafeInteger(this.timeoutMs) || this.timeoutMs < 1 || this.timeoutMs > 120000)
            throw new RangeError("invalid connection timeout");
        this.maxMessageBytes = messageLimit(options?.maxMessageBytes);
        this.now = options?.now;
        if (this.now !== undefined && typeof this.now !== "function") throw new TypeError("invalid clock");
        this.transportOptions = copyTransport(options?.transport, "answerer");
        this.identity = copyIdentity(identity);
        this.module = module; this.proxy = proxy; this.watch = watch; this.handlers = handlers;
        this.state = "new"; this.epoch = 0; this.connection = null; this.outbox = null; this.authenticated = false;
        this.signalingUnavailable = false;
        try {
            this.listener = new BrowserListener(module, proxy, this.identity, watch, this.listenerHandlers(), options.listener);
            if (!matchingIdentity(this.listener.lease, watch)) throw new Error("watch and local identity do not match");
        } catch (error) { this.dispose(); throw error; }
    }
    listenerHandlers() {
        return { onPublished: () => {
            if (this.state === "starting") this.state = "listening";
            this.notify("onPublished");
        }, onRequest: (peer) => {
            if (!["starting", "listening"].includes(this.state)) return;
            this.state = "pending"; this.notify("onState", "pending"); this.notify("onRequest", peer);
        }, onStatus: (reason) => this.notify("onStatus", reason),
        onError: (reason, detail) => this.signalFailure(reason, detail) };
    }
    connectionHandlers() {
        return { onSignal: (wire) => {
                if (!this.signalingUnavailable && this.outbox.send(wire) !== "accepted") throw new Error("signaling queue failed");
            },
            onState: (value) => {
                if (value === "failed") return;
                if (value === "ready" && !this.authenticated) { this.authenticated = true; this.listener.lease.close(); }
                this.state = value; this.notify("onState", value);
            }, onRoute: (value) => this.notify("onRoute", value), onMessage: (value) => this.notify("onMessage", value),
            onWritable: () => this.notify("onWritable"), onError: (reason) => this.fail(reason) };
    }
    async start() {
        if (this.state !== "new") throw new Error("incoming listener cannot be started");
        this.state = "starting";
        try {
            const started = await this.listener.start();
            if (!started && this.state === "starting") this.fail("listener");
            return started && !["failed", "closed"].includes(this.state);
        } catch { this.fail("listener"); return false; }
    }
    retry() { return this.listener.retry(); }
    async accept() {
        if (this.state !== "pending") return false;
        const offer = this.listener.accept((wire) => this.receive(wire));
        if (!offer) { if (this.listener.state !== "pending") this.fail("stale"); return false; }
        try {
            this.outbox = new ProxyOutbox(this.proxy, offer.peer.inbox,
                { onPublished: () => {}, onError: (error) => this.signalFailure("signaling", error) });
            this.connection = new BrowserConnection(this.module, { secure: { ...this.identity, role: "answerer",
                timeoutMs: this.timeoutMs, maxMessageBytes: this.maxMessageBytes,
                remoteSigningKey: offer.peer.signingKey, remoteBoxKey: offer.peer.recipientKey,
                localInstance: offer.local.instance, remoteInstance: offer.peer.instance, attemptId: offer.attemptId },
                transport: this.transportOptions, now: this.now }, this.connectionHandlers());
            this.wipe(); this.state = "connecting"; this.notify("onState", "connecting");
            await this.connection.start();
            if (this.state !== "connecting") return false;
            if (this.connection.receive(offer.offer) !== "accepted" || !this.listener.resume()) {
                this.fail("signaling"); return false;
            }
            return true;
        } catch { this.fail("connection"); return false; }
    }
    receive(wire) {
        const result = this.connection.receive(wire);
        if (result !== "accepted" && result !== "ignored") this.fail("signaling");
    }
    signalFailure(reason, error) {
        if (["closed", "failed"].includes(this.state)) return;
        if (!this.connection?.authenticated || !isTransientProxyError(error)) { this.fail(reason); return; }
        if (this.signalingUnavailable) return;
        this.signalingUnavailable = true; this.listener.close(); this.outbox?.close();
        this.notify("onStatus", "signalingUnavailable");
    }
    reject() { if (this.state !== "pending") return false; this.close(); return true; }
    notify(name, value) {
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (epoch !== this.epoch || ["closed", "failed"].includes(this.state)) return;
            if (name === "onRequest" && this.state !== "pending") return;
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
    dispose() { this.wipe(); this.outbox?.close(); this.connection?.close(); this.listener?.close(); }
    fail(reason) {
        if (["closed", "failed"].includes(this.state)) return;
        this.state = "failed"; ++this.epoch; this.dispose();
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (this.state !== "failed" || epoch !== this.epoch) return;
            try { this.handlers.onState("failed"); } catch (error) { console.error("p2pdc: incoming state callback failed", error); }
            if (this.state !== "failed" || epoch !== this.epoch) return;
            try { this.handlers.onError(reason); } catch (error) { console.error("p2pdc: incoming error callback failed", error); }
        });
    }
    close() { if (this.state !== "closed") { this.state = "closed"; ++this.epoch; this.dispose(); } }
}
