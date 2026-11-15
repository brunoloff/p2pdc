import { WasmDiscovery } from "./wasm_discovery.mjs";
import { ProxyOutbox } from "./proxy_outbox.mjs";
import { timing } from "./discovery_clock.mjs";

// Bounded announcement lease. Each successful publication announces an
// ephemeral endpoint for 120 seconds; refresh occurs after 60 seconds.
// A publication being accepted by the proxy does not imply peer observation.
// Start only after the owner is prepared to receive on the endpoint inbox.
export class BrowserLease {
    constructor(module, proxy, identity, handlers, options = {}) {
        if (!proxy || typeof proxy.put !== "function" || !handlers ||
            typeof handlers.onPublished !== "function" || typeof handlers.onError !== "function")
            throw new TypeError("invalid lease callbacks");
        this.handlers = handlers; this.retryMs = options.retryMs ?? 2000;
        this.maxPublications = options.maxPublications ?? 32;
        this.windowed = options.windowed ?? true;
        if (!Number.isSafeInteger(this.retryMs) || this.retryMs < 50 || this.retryMs > 60000 ||
            !Number.isSafeInteger(this.maxPublications) || this.maxPublications < 1 || this.maxPublications > 32 ||
            typeof this.windowed !== "boolean")
            throw new TypeError("invalid lease limits");
        ({ timers: this.timers, clock: this.clock } = timing(options));
        this.closed = false; this.failed = false; this.started = false; this.pending = false;
        this.timerGeneration = 0;
        this.timer = null; this.attempts = 0; this.expires = 0; this.refreshAt = 0; this.publishedExpiry = 0;
        this.proxy = proxy; this.window = null; this.outbox = null;
        this.discovery = new WasmDiscovery(module, identity, { onPeer: () => {}, onError: () => this.fail("discovery") });
        try {
            if (!this.windowed) this.outbox = this.makeOutbox(this.discovery.key());
        } catch (error) { this.discovery.close(); throw error; }
    }
    makeOutbox(key) {
        if (!key) throw new Error("cannot derive group key");
        return new ProxyOutbox(this.proxy, key, { onPublished: () => this.completed(), onError: () => this.retryLater() },
            { maxQueuedRecords: 1, maxQueuedBytes: 32768, maxRecordBytes: 32768 });
    }
    prepareWindow(now) {
        if (!this.windowed) return true;
        const window = Math.floor(now / 120);
        if (window === this.window) return true;
        try {
            const outbox = this.makeOutbox(this.discovery.windowKey(now));
            this.outbox?.close(); this.outbox = outbox; this.window = window; this.attempts = 0;
            return true;
        } catch { this.fail("discovery"); return false; }
    }
    start() {
        if (this.started || this.closed || this.failed) throw new Error("lease cannot be started");
        this.started = true;
        return this.publish();
    }
    publish() {
        if (this.closed || this.failed || this.pending) return false;
        let now;
        try { now = this.clock.read(); }
        catch { this.fail("clock"); return false; }
        if (!this.prepareWindow(now)) return false;
        if (this.attempts >= this.maxPublications) { this.fail("publicationLimit"); return false; }
        let wire;
        try { wire = this.windowed ? this.discovery.publishWindowed(now) : this.discovery.publish(now); }
        catch { this.fail("discovery"); return false; }
        if (!wire) { this.fail("discovery"); return false; }
        this.pending = true; this.expires = now + 120; this.refreshAt = now + 60; ++this.attempts;
        if (this.outbox.send(wire) !== "accepted") { this.fail("resource"); return false; }
        this.schedule(120000, "expire");
        return !this.closed && !this.failed;
    }
    completed() {
        if (this.closed || this.failed) return;
        let now;
        try { now = this.clock.read(); }
        catch { this.fail("clock"); return; }
        if (!this.pending || now >= this.expires) { this.fail("expiredPending"); return; }
        this.clearTimer();
        this.pending = false; this.publishedExpiry = this.expires;
        try { this.handlers.onPublished(); }
        catch { this.fail("callback"); return; }
        if (!this.closed && !this.failed) this.schedule(Math.max(0, (this.refreshAt - now) * 1000), "refresh");
    }
    retryLater() {
        if (this.closed || this.failed) return;
        if (this.attempts >= this.maxPublications) { this.fail("publicationLimit"); return; }
        let now;
        try { now = this.clock.read(); }
        catch { this.fail("clock"); return; }
        if (!this.pending || now >= this.expires) { this.fail("expiredPending"); return; }
        this.clearTimer();
        try { this.handlers.onError("retrying"); }
        catch { this.fail("callback"); return; }
        if (!this.closed && !this.failed) this.schedule(Math.min(this.retryMs, (this.expires - now) * 1000), "retry");
    }
    schedule(delay, action) {
        if (this.closed || this.failed || this.timer !== null) return;
        const generation = ++this.timerGeneration;
        try {
            const timer = this.timers.set(() => {
            if (generation !== this.timerGeneration) return;
            this.timer = null; this.tick(action);
            }, delay);
            if (timer == null) throw new Error("timer returned no handle");
            this.timer = timer;
        }
        catch { this.fail("scheduler"); }
    }
    tick(action) {
        if (this.closed || this.failed) return;
        let now;
        try { now = this.clock.read(); }
        catch { this.fail("clock"); return; }
        if (action === "refresh") {
            if (now < this.refreshAt) this.schedule((this.refreshAt - now) * 1000, "refresh");
            else this.publish();
            return;
        }
        if (!this.pending || now >= this.expires) { this.fail("expiredPending"); return; }
        if (action === "expire") { this.schedule((this.expires - now) * 1000, "expire"); return; }
        if (this.attempts >= this.maxPublications) { this.fail("publicationLimit"); return; }
        ++this.attempts;
        if (!this.outbox.retry()) { this.fail("backend"); return; }
        this.schedule((this.expires - now) * 1000, "expire");
    }
    self() {
        if (this.closed || this.failed || !this.publishedExpiry) return null;
        try { return this.clock.read() < this.publishedExpiry ? this.discovery.self() : null; }
        catch { this.fail("clock"); return null; }
    }
    endpoint() { return !this.closed && !this.failed ? this.discovery.self() : null; }
    healthy() { return this.started && !this.closed && !this.failed && this.outbox.healthy(); }
    fail(reason) {
        if (this.closed || this.failed) return;
        this.failed = true; this.stop();
        queueMicrotask(() => {
            if (this.closed) return;
            try { this.handlers.onError(reason); }
            catch { console.error("p2pdc: lease error callback failed"); }
        });
    }
    stop() {
        this.clearTimer();
        this.outbox?.close(); this.discovery.close();
    }
    clearTimer() {
        if (this.timer === null) return;
        ++this.timerGeneration;
        const timer = this.timer; this.timer = null;
        try { this.timers.clear(timer); } catch (error) { console.error("p2pdc: lease timer cancellation failed", error); }
    }
    close() { if (!this.closed) { this.closed = true; this.stop(); } }
}
