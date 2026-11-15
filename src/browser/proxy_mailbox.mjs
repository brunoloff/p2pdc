import { keyHex, ProxyError, sameBytes } from "./proxy_codec.mjs";
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;

function limits(options) {
    const value = { records: options.maxQueuedRecords ?? 128, bytes: options.maxQueuedBytes ?? 1048576,
                    record: options.maxRecordBytes ?? 32768 };
    if (!Number.isSafeInteger(value.records) || value.records < 1 || value.records > 1024 ||
        !Number.isSafeInteger(value.bytes) || value.bytes < 1 || value.bytes > 16777216 ||
        !Number.isSafeInteger(value.record) || value.record < 1 || value.record > 32768 || value.record > value.bytes)
        throw new TypeError("invalid mailbox limits");
    return value;
}

// Raw mailbox. The owner verifies every record and handles duplicates, replay,
// ordering and timeouts. Stream headers are not a DHT registration guarantee.
export class ProxyMailbox {
    constructor(proxy, key, handlers, options = {}) {
        if (!proxy || typeof proxy.listen !== "function" || typeof proxy.get !== "function" ||
            !handlers || typeof handlers.onRecord !== "function" || typeof handlers.onError !== "function" ||
            typeof handlers.onCatchup !== "function") throw new TypeError("invalid mailbox dependencies");
        if (!(key instanceof Uint8Array) || byteLength.call(key) !== 20) throw new TypeError("invalid mailbox key");
        this.key = new Uint8Array(20); Uint8Array.prototype.set.call(this.key, key); keyHex(this.key);
        this.proxy = proxy; this.handlers = handlers; this.limits = limits(options);
        this.deduplicateByContent = options.deduplicateByContent ?? false;
        if (typeof this.deduplicateByContent !== "boolean") throw new TypeError("invalid lookup deduplication option");
        this.controller = new AbortController(); this.subscription = null; this.started = false;
        this.closed = false; this.failed = false; this.error = null; this.queue = []; this.bytes = 0; this.scheduled = false;
    }
    async start() {
        if (this.started || this.closed || this.failed) throw new Error("mailbox cannot be started");
        this.started = true;
        try {
            this.subscription = this.proxy.listen(this.key, (value) => this.ingest(value), this.controller.signal);
            void this.subscription.done.then(() => {
                if (!this.closed && !this.failed) this.fail(new ProxyError("stream_closed", "mailbox stream ended"));
            }, (error) => { if (!this.closed && !this.failed) this.fail(error); });
            if (!await this.subscription.ready || this.closed || this.failed) return false;
            const records = this.deduplicateByContent ?
                await this.proxy.get(this.key, this.controller.signal, { deduplicateByContent: true }) :
                await this.proxy.get(this.key, this.controller.signal);
            if (this.closed || this.failed) return false;
            for (const record of records) if (!this.ingest(record)) return false;
            this.emitCatchup();
            return true;
        } catch (error) {
            if (!this.closed && !this.failed) this.fail(error);
            return false;
        }
    }
    ingest(record) {
        if (this.closed || this.failed) return false;
        if (!record || !(record.bytes instanceof Uint8Array) || typeof record.expired !== "boolean") {
            this.fail(new TypeError("invalid mailbox record")); return false;
        }
        if (record.expired) return true;
        const size = byteLength.call(record.bytes);
        if (size <= this.limits.record && this.deduplicateByContent &&
            this.queue.some((queued) => sameBytes(queued, record.bytes))) return true;
        if (size > this.limits.record || this.queue.length >= this.limits.records || size > this.limits.bytes - this.bytes) {
            this.fail(new RangeError("mailbox receive limit exceeded")); return false;
        }
        try {
            const copy = new Uint8Array(size);
            Uint8Array.prototype.set.call(copy, record.bytes);
            this.queue.push(copy);
        }
        catch (error) { this.fail(error); return false; }
        this.bytes += size;
        this.schedule();
        return true;
    }
    schedule() {
        if (this.scheduled || this.closed || this.failed) return;
        this.scheduled = true;
        queueMicrotask(() => { this.scheduled = false; this.drain(); });
    }
    drain() {
        for (let count = 0; count < 16 && this.queue.length && !this.closed && !this.failed; ++count) {
            const record = this.queue.shift(); this.bytes -= record.length;
            try { this.handlers.onRecord(record); }
            catch (error) { this.fail(error); }
        }
        if (this.queue.length) this.schedule();
    }
    emitCatchup() {
        queueMicrotask(() => {
            if (this.closed || this.failed) return;
            try { this.handlers.onCatchup(); }
            catch (error) { this.fail(error); }
        });
    }
    fail(error) {
        if (this.closed || this.failed) return;
        this.failed = true; this.error = error; this.controller.abort(); this.subscription?.close();
        this.queue.length = 0; this.bytes = 0;
        queueMicrotask(() => {
            if (this.closed) return;
            try { this.handlers.onError(error); }
            catch (failure) { console.error("p2pdc: mailbox error callback failed", failure); }
        });
    }
    close() {
        if (this.closed) return;
        this.closed = true; this.controller.abort(); this.subscription?.close();
        this.queue.length = 0; this.bytes = 0;
    }
    healthy() { return this.started && !this.closed && !this.failed; }
}
