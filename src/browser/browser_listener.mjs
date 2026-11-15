import { BrowserLease } from "./browser_lease.mjs";
import { ProxyMailbox } from "./proxy_mailbox.mjs";
import { WasmIncoming } from "./wasm_incoming.mjs";
import { timing } from "./discovery_clock.mjs";

const copyEndpoint = (value) => ({ signingKey: value.signingKey.slice(), recipientKey: value.recipientKey.slice(),
    instance: value.instance.slice(), inbox: value.inbox.slice() });
const copyPeer = (value) => ({ ...value, ...copyEndpoint(value) });
const equalBytes = (a, b) => a.length === b.length && a.every((value, index) => value === b[index]);
const sameEndpoint = (a, b) => a && b && ["signingKey", "recipientKey", "instance", "inbox"]
    .every((name) => equalBytes(a[name], b[name]));

// Single-use internal listener. Owns the lease and mailbox until close; the
// caller owns the watch and must call retry() after discovery changes. While
// discovery recovers, bounded untrusted records wait for reinspection.
export class BrowserListener {
    constructor(module, proxy, identity, watch, handlers, options = {}) {
        for (const name of ["onPublished", "onRequest", "onStatus", "onError"]) {
            if (typeof handlers?.[name] !== "function") throw new TypeError(`missing ${name} callback`);
        }
        this.timeoutMs = options.requestTimeoutMs ?? 30000;
        if (!Number.isSafeInteger(this.timeoutMs) || this.timeoutMs < 100 || this.timeoutMs > 30000)
            throw new RangeError("invalid request timeout");
        ({ timers: this.timers } = timing(options));
        this.deadlineNow = options.deadlineNow ?? (() => Date.now());
        if (typeof this.deadlineNow !== "function") throw new TypeError("invalid deadline clock");
        this.lastDeadlineTime = null; this.deadline = null;
        this.handlers = handlers; this.watch = watch; this.state = "new"; this.epoch = 0;
        this.timer = null; this.timerGeneration = 0; this.pending = null; this.sink = null;
        this.queue = []; this.bytes = 0; this.scheduled = false; this.resumed = false;
        this.lease = new BrowserLease(module, proxy, identity, {
            onPublished: () => this.published(), onError: (reason) => this.leaseError(reason),
        }, options);
        try {
            if (this.lease.windowed !== watch?.windowed) throw new TypeError("discovery protocols must match");
            this.incoming = new WasmIncoming(this.lease, watch, options);
            const endpoint = this.lease.endpoint();
            if (!endpoint) throw new Error("local endpoint unavailable");
            this.mailbox = new ProxyMailbox(proxy, endpoint.inbox, {
                onRecord: (wire) => this.record(wire), onCatchup: () => {}, onError: (error) => this.fail("mailbox", error),
            });
        } catch (error) { this.incoming?.close(); this.lease.close(); throw error; }
    }
    async start() {
        if (this.state !== "new") throw new Error("listener cannot be started");
        this.state = "starting";
        if (!await this.mailbox.start() || this.state !== "starting") {
            if (this.state === "starting") this.fail("mailbox");
            return false;
        }
        if (!this.lease.start()) { this.fail("lease"); return false; }
        return this.state !== "failed" && this.state !== "closed";
    }
    published() {
        if (!["starting", "listening", "pending", "accepted"].includes(this.state)) return;
        if (this.state === "starting") this.state = "listening";
        this.notify("onPublished");
        if (this.state === "listening") this.retry();
    }
    leaseError(reason) {
        if (reason === "retrying") this.notify("onStatus", reason);
        else this.fail(reason);
    }
    notify(name, value) {
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (epoch !== this.epoch || ["closed", "failed"].includes(this.state)) return;
            if (name === "onRequest" && this.state !== "pending") return;
            try { this.handlers[name](value); } catch { this.fail("callback"); }
        });
    }
    record(wire) {
        if (["closed", "failed"].includes(this.state)) return;
        if ((this.pending && equalBytes(this.pending.offer, wire)) ||
            this.queue.some((entry) => equalBytes(entry.wire, wire))) return;
        if (this.state === "pending" && !this.watch.recovering?.()) {
            const result = this.incoming.receive(wire, true);
            if (result.status === "failed" || result.status === "closed" || result.status === "notReady") {
                this.fail("admission"); return;
            }
            if (result.status !== "nonOffer") return;
        }
        if (this.queue.length >= 64 || wire.length > 524288 - this.bytes) { this.fail("backpressure"); return; }
        this.queue.push({ wire: wire.slice(), check: this.state !== "accepted" }); this.bytes += wire.length;
        if (this.state === "accepted") this.scheduleDelivery();
        else if (this.state === "listening") this.inspect(this.queue.length - 1);
    }
    retry() {
        if (this.state === "pending") return this.retryPending();
        if (this.state !== "listening") return false;
        for (let i = 0; i < this.queue.length && this.state === "listening";) {
            if (!this.queue[i].check || !this.inspect(i)) ++i;
        }
        return this.state !== "failed" && this.state !== "closed";
    }
    retryPending() {
        for (let index = 0; index < this.queue.length;) {
            const entry = this.queue[index];
            if (!entry.check) { ++index; continue; }
            const result = this.incoming.receive(entry.wire, true);
            if (result.status === "notReady") {
                if (!this.watch.recovering?.()) this.fail("stale");
                return false;
            }
            if (result.status === "failed" || result.status === "closed") { this.fail("admission"); return false; }
            if (result.status === "nonOffer") { entry.check = false; ++index; }
            else { this.queue.splice(index, 1); this.bytes -= entry.wire.length; }
        }
        return this.state === "pending";
    }
    inspect(index) {
        const entry = this.queue[index], result = this.incoming.receive(entry.wire);
        if (result.status === "failed" || result.status === "closed") { this.fail("admission"); return false; }
        if (result.status === "ignored") {
            this.queue.splice(index, 1); this.bytes -= entry.wire.length; return true;
        }
        if (result.status === "nonOffer") entry.check = false;
        if (result.status !== "admitted") return false;
        this.queue.splice(index, 1); this.bytes -= entry.wire.length;
        const local = this.lease.self();
        if (!local) { this.fail("stale"); return true; }
        this.pending = { peer: copyPeer(result.peer), local: copyEndpoint(local),
            attemptId: result.attemptId.slice(), offer: result.wire.slice() };
        this.state = "pending";
        this.armTimeout();
        if (this.state === "pending") this.notify("onRequest", copyPeer(this.pending.peer));
        return true;
    }
    armTimeout() {
        try { this.deadline = this.deadlineTime() + this.timeoutMs; }
        catch { this.fail("clock"); return; }
        this.scheduleTimeout(this.timeoutMs);
    }
    deadlineTime() {
        const value = this.deadlineNow();
        if (!Number.isSafeInteger(value) || value < 0 || value > Number.MAX_SAFE_INTEGER - 30000 ||
            (this.lastDeadlineTime !== null && value < this.lastDeadlineTime)) throw new RangeError("invalid clock");
        this.lastDeadlineTime = value; return value;
    }
    scheduleTimeout(delay) {
        const generation = ++this.timerGeneration;
        try {
            this.timer = this.timers.set(() => {
                if (generation !== this.timerGeneration) return;
                this.timer = null; this.timeoutTick();
            }, delay);
            if (this.timer == null) throw new Error("timer returned no handle");
        } catch { this.fail("scheduler"); }
    }
    timeoutTick() {
        try {
            const remaining = this.deadline - this.deadlineTime();
            if (remaining > 0) this.scheduleTimeout(remaining); else this.fail("timeout");
        } catch { this.fail("clock"); }
    }
    clearTimer() {
        if (this.timer === null) return;
        ++this.timerGeneration;
        const timer = this.timer; this.timer = null;
        try { this.timers.clear(timer); } catch (error) { console.error("p2pdc: listener timer cancellation failed", error); }
    }
    accept(onRecord) {
        if (typeof onRecord !== "function") throw new TypeError("record callback required");
        if (this.state !== "pending") return null;
        try { if (this.deadlineTime() >= this.deadline) { this.fail("timeout"); return null; } }
        catch { this.fail("clock"); return null; }
        if (this.watch.recovering?.()) return null;
        if (!this.retryPending()) return null;
        const peer = this.watch.current(this.pending.peer.handle), local = this.lease.self();
        if (this.lease.discovery !== this.incoming.localDiscovery || this.watch.discovery !== this.incoming.watchDiscovery ||
            this.lease.discovery.id !== this.incoming.localId || this.watch.discovery.id !== this.incoming.watchId ||
            !this.watch.healthy() || !peer || !sameEndpoint(peer, this.pending.peer) ||
            !sameEndpoint(local, this.pending.local)) {
            this.fail("stale"); return null;
        }
        this.clearTimer(); this.state = "accepted"; this.sink = onRecord;
        const result = { peer: copyPeer(peer), local: copyEndpoint(local), attemptId: this.pending.attemptId.slice(),
            offer: this.pending.offer.slice() };
        this.pending = null; return result;
    }
    resume() {
        if (this.state !== "accepted" || this.resumed) return false;
        this.resumed = true; this.scheduleDelivery(); return true;
    }
    scheduleDelivery() {
        if (this.scheduled || this.state !== "accepted" || !this.resumed || !this.queue.length) return;
        this.scheduled = true;
        queueMicrotask(() => { this.scheduled = false; this.deliver(); });
    }
    deliver() {
        for (let i = 0; i < 16 && this.state === "accepted" && this.queue.length; ++i) {
            const entry = this.queue.shift(); this.bytes -= entry.wire.length;
            try { this.sink(entry.wire); } catch { this.fail("callback"); }
        }
        this.scheduleDelivery();
    }
    fail(reason, detail) {
        if (["closed", "failed"].includes(this.state)) return;
        this.state = "failed"; ++this.epoch; this.stop();
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (this.state !== "failed" || this.epoch !== epoch) return;
            try { this.handlers.onError(reason, detail); } catch (error) { console.error("p2pdc: listener error callback failed", error); }
        });
    }
    stop() {
        this.clearTimer(); this.mailbox.close(); this.incoming.close(); this.lease.close();
        this.queue.length = 0; this.bytes = 0; this.pending = null; this.sink = null;
    }
    close() { if (this.state !== "closed") { this.state = "closed"; ++this.epoch; this.stop(); } }
}
