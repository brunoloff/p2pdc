import { ProxyError, isTransientProxyError, keyHex, sameBytes } from "./proxy_codec.mjs";
import { ProxyMailbox } from "./proxy_mailbox.mjs";

// Discovery only: signaling mailboxes retain their existing terminal behavior.
// Lookup follows each newly opened stream; neither proves network completeness.
export class RecoveringMailbox {
    constructor(proxy, key, handlers, timers, options = {}) {
        if (!proxy || typeof proxy.listen !== "function" || typeof proxy.get !== "function" ||
            !timers || typeof timers.set !== "function" || typeof timers.clear !== "function")
            throw new TypeError("invalid recovery dependencies");
        keyHex(key);
        this.proxy = proxy; this.key = key; this.handlers = handlers; this.timers = timers;
        this.maxRetries = options.maxRetries ?? 5; this.retryDelay = options.retryDelayMs ?? 1000;
        if (!Number.isSafeInteger(this.maxRetries) || this.maxRetries < 0 || this.maxRetries > 10 ||
            !Number.isSafeInteger(this.retryDelay) || this.retryDelay < 50 || this.retryDelay > 60000)
            throw new TypeError("invalid discovery recovery limits");
        this.state = "new"; this.closed = false; this.failed = false; this.current = null; this.everReady = false;
        this.timer = null; this.retries = 0; this.records = []; this.bytes = 0; this.caught = false;
    }
    async start() {
        if (this.state !== "new") throw new Error("mailbox cannot be started");
        this.state = "opening";
        return this.open(false);
    }
    async open(recovering) {
        this.records = []; this.bytes = 0; this.caught = false;
        let box;
        try {
            box = new ProxyMailbox(this.proxy, this.key, {
                onRecord: (record) => this.record(box, record),
                onCatchup: () => this.catchup(box), onError: (error) => this.error(box, error),
            }, { deduplicateByContent: true, maxQueuedRecords: 512 });
            this.current = box;
            const ready = await box.start();
            if (this.closed || this.failed || this.current !== box) return false;
            if (!ready || !box.healthy()) {
                this.interrupted(box.error ?? new ProxyError("network_error", "proxy stream failed"), recovering);
                return false;
            }
            this.state = "ready"; this.everReady = true; this.retries = 0;
            if (recovering) this.handlers.onStatus("ready");
            if (!this.closed && !this.failed) await this.flush(box);
            return !this.closed && !this.failed && this.current === box;
        } catch (error) {
            if (!this.closed && !this.failed) this.interrupted(error, recovering);
            return false;
        }
    }
    record(box, value) {
        if (box !== this.current || this.closed || this.failed) return;
        if (this.state === "ready") { this.handlers.onRecord(value); return; }
        if (this.records.some((queued) => sameBytes(queued, value))) return;
        if (this.records.length >= 512 || value.length > 1048576 - this.bytes) {
            this.fail(new RangeError("discovery recovery buffer exceeded")); return;
        }
        this.records.push(value); this.bytes += value.length;
    }
    catchup(box) {
        if (box !== this.current || this.closed || this.failed) return;
        if (this.state === "ready") this.handlers.onCatchup();
        else this.caught = true;
    }
    async flush(box) {
        const records = this.records, caught = this.caught;
        this.records = []; this.bytes = 0; this.caught = false;
        for (let index = 0; index < records.length; ++index) {
            if (this.closed || this.failed || this.current !== box) return;
            this.handlers.onRecord(records[index]);
            if ((index + 1) % 16 === 0) await Promise.resolve();
        }
        if (caught && !this.closed && !this.failed && this.current === box) this.handlers.onCatchup();
    }
    error(box, error) {
        if (box !== this.current || this.closed || this.failed || this.state !== "ready") return;
        this.interrupted(error, true);
    }
    interrupted(error, recovering) {
        this.current?.close(); this.current = null; this.records = []; this.bytes = 0;
        if (!recovering || !isTransientProxyError(error) || this.retries >= this.maxRetries) { this.fail(error); return; }
        const wasReady = this.state === "ready";
        this.state = "waiting";
        if (wasReady) this.handlers.onStatus("reconnecting");
        if (this.closed || this.failed) return;
        const delay = Math.min(this.retryDelay * 2 ** this.retries++, 60000);
        try {
            this.timer = this.timers.set(() => {
                if (this.closed || this.failed || this.state !== "waiting") return;
                this.timer = null; this.state = "opening"; void this.open(true);
            }, delay);
            if (this.timer == null) throw new Error("timer returned no handle");
        } catch (failure) { this.fail(failure); }
    }
    fail(error) {
        if (this.closed || this.failed) return;
        this.failed = true; this.state = "failed"; this.current?.close(); this.current = null;
        this.records = []; this.bytes = 0; this.handlers.onError(error);
    }
    healthy() {
        return (this.state === "ready" || (this.state === "opening" && !this.everReady)) &&
            !this.closed && !this.failed && this.current?.healthy() === true;
    }
    recovering() {
        if (this.closed || this.failed || !this.everReady) return false;
        if (["waiting", "opening"].includes(this.state)) return true;
        return this.state === "ready" && this.current?.failed === true &&
            this.retries < this.maxRetries && isTransientProxyError(this.current.error);
    }
    close() {
        if (this.closed) return;
        this.closed = true; this.state = "closed"; this.current?.close(); this.current = null;
        if (this.timer !== null) {
            const timer = this.timer; this.timer = null;
            try { this.timers.clear(timer); } catch (error) { console.error("p2pdc: recovery timer cancellation failed", error); }
        }
        this.records = []; this.bytes = 0;
    }
}
