import { encodeBase64, ProxyError } from "./proxy_codec.mjs";

// Lookup snapshots are best effort: absent values are not treated as expired.
// A cache suppresses repeat delivery without assuming that record IDs are unique.
export class PollingProxyWatch {
    constructor(proxy, key, emit, cache, interval) {
        this.proxy = proxy; this.key = key; this.emit = emit; this.cache = cache; this.interval = interval;
        this.controller = new AbortController(); this.closed = false; this.previous = new Set();
        this.cacheBytes = 0; this.timer = null; this.wake = null;
        this.ready = new Promise((resolve) => { this.resolveReady = resolve; });
        this.done = this.run();
    }
    update(records) {
        const next = new Set(), changes = [];
        let bytes = 0;
        for (const record of records) {
            const id = record.id || "";
            const fingerprint = `${id.length}:${id}:${record.expired ? 1 : 0}:${encodeBase64(record.bytes)}`;
            if (next.has(fingerprint)) continue;
            next.add(fingerprint); bytes += fingerprint.length * 2;
            if (!this.previous.has(fingerprint)) changes.push(record);
        }
        this.cache.replace(this.cacheBytes, bytes);
        this.previous = next; this.cacheBytes = bytes;
        for (const record of changes) { if (!this.closed) this.emit(record); }
    }
    pause() {
        return new Promise((resolve) => {
            this.wake = resolve;
            this.timer = setTimeout(() => { this.timer = null; this.wake = null; resolve(); }, this.interval);
        });
    }
    async run() {
        try {
            while (!this.closed) {
                const records = await this.proxy.get(this.key, this.controller.signal,
                                                     { deduplicateByContent: true });
                if (this.closed) break;
                this.resolveReady(true);
                this.update(records);
                if (!this.closed) await this.pause();
            }
        } catch (error) {
            if (!this.closed) throw error;
        } finally {
            this.resolveReady(false);
            this.release();
        }
    }
    release() {
        this.cache.replace(this.cacheBytes, 0);
        this.cacheBytes = 0; this.previous.clear();
    }
    close() {
        if (this.closed) return;
        this.closed = true; this.controller.abort();
        if (this.timer !== null) clearTimeout(this.timer);
        this.timer = null;
        if (this.wake) { const wake = this.wake; this.wake = null; wake(); }
        this.release();
    }
}

export class ProxyWatchCache {
    constructor(limit) { this.limit = limit; this.used = 0; }
    replace(oldSize, newSize) {
        if (oldSize > this.used || newSize > this.limit - (this.used - oldSize))
            throw new ProxyError("resource_limit", "proxy watch cache exceeded limits");
        this.used += newSize - oldSize;
    }
}
