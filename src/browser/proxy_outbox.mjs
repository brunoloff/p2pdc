import { keyHex } from "./proxy_codec.mjs";
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;

function bounds(options) {
    const result = { records: options.maxQueuedRecords ?? 64, bytes: options.maxQueuedBytes ?? 524288,
                     record: options.maxRecordBytes ?? 32768 };
    if (!Number.isSafeInteger(result.records) || result.records < 1 || result.records > 1024 ||
        !Number.isSafeInteger(result.bytes) || result.bytes < 1 || result.bytes > 16777216 ||
        !Number.isSafeInteger(result.record) || result.record < 1 || result.record > 32768 || result.record > result.bytes)
        throw new TypeError("invalid outbox limits");
    return result;
}

// Raw bounded FIFO. Failed publications retain their original bytes and pause
// until retry. Publication success does not establish remote delivery.
export class ProxyOutbox {
    constructor(proxy, key, handlers, options = {}) {
        if (!proxy || typeof proxy.put !== "function" || !handlers ||
            typeof handlers.onPublished !== "function" || typeof handlers.onError !== "function")
            throw new TypeError("invalid outbox dependencies");
        if (!(key instanceof Uint8Array) || byteLength.call(key) !== 20) throw new TypeError("invalid outbox key");
        this.key = new Uint8Array(20); Uint8Array.prototype.set.call(this.key, key); keyHex(this.key);
        this.proxy = proxy; this.handlers = handlers; this.limits = bounds(options);
        this.controller = new AbortController(); this.closed = false; this.paused = false; this.scheduled = false;
        this.notifying = false;
        this.queue = []; this.bytes = 0;
    }
    send(value) {
        if (this.closed) return "closed";
        if (!(value instanceof Uint8Array)) throw new TypeError("binary record required");
        const size = byteLength.call(value);
        if (size > this.limits.record) return "tooLarge";
        if (this.queue.length >= this.limits.records || size > this.limits.bytes - this.bytes) return "backpressure";
        try {
            const copy = new Uint8Array(size);
            Uint8Array.prototype.set.call(copy, value);
            this.queue.push(copy);
        }
        catch (error) { this.report(error); return "failed"; }
        this.bytes += size; this.schedule();
        return "accepted";
    }
    schedule() {
        if (this.closed || this.paused || this.scheduled || !this.queue.length) return;
        this.scheduled = true;
        queueMicrotask(() => { void this.publish(); });
    }
    async publish() {
        try {
            if (this.closed || this.paused || !this.queue.length) return;
            await this.proxy.put(this.key, this.queue[0], this.controller.signal);
            if (this.closed) return;
            const value = this.queue.shift(); this.bytes -= value.length;
            try { this.handlers.onPublished(); }
            catch (error) { if (!this.closed) { this.report(error); this.close(); } }
        } catch (error) {
            if (!this.closed) { this.paused = true; this.report(error); }
        } finally {
            this.scheduled = false;
            this.schedule();
        }
    }
    report(error) {
        this.notifying = true;
        try { this.handlers.onError(error); }
        catch (failure) { this.close(); console.error("p2pdc: outbox error callback failed", failure); }
        finally { this.notifying = false; }
    }
    retry() {
        if (this.closed || !this.paused || this.notifying) return false;
        this.paused = false; this.schedule(); return true;
    }
    close() {
        if (this.closed) return;
        this.closed = true; this.controller.abort(); this.queue.length = 0; this.bytes = 0;
    }
    healthy() { return !this.closed; }
}
