import { encodeBase64 } from "./proxy_codec.mjs";

// How long backends that set their own expiry keep a value: as long as OpenDHT does.
export const recordLifetimeSeconds = 600;

// A random identifier for a value, request or connection: 16 bytes in hex.
export function randomId() {
    return Array.from(crypto.getRandomValues(new Uint8Array(16)), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

// Limits every browser record backend applies to what it reads and how long it waits.
// Records beyond them are untrusted surplus and are dropped.
export function storeLimits(options = {}) {
    const result = { maxValueBytes: options.maxValueBytes ?? 32768,
                     maxValues: options.maxValues ?? 512,
                     maxTotalBytes: options.maxTotalBytes ?? 1048576,
                     timeoutMs: options.timeoutMs ?? 10000,
                     listenIdleMs: options.listenIdleMs ?? 300000,
                     maxRecordsPerSecond: options.maxRecordsPerSecond ?? 256 };
    for (const value of Object.values(result)) {
        if (!Number.isSafeInteger(value) || value <= 0) throw new TypeError("invalid proxy limits");
    }
    if (result.maxValueBytes > 32768 || result.maxValues > 1024 ||
        result.maxTotalBytes > 16 * 1024 * 1024 || result.timeoutMs > 600000 ||
        result.listenIdleMs > 3600000 || result.maxRecordsPerSecond > 65536) {
        throw new TypeError("proxy limits exceed supported bounds");
    }
    return result;
}

// Admits at most perSecond records per one-second window; the rest are dropped
// so a hostile proxy cannot flood the WebAssembly heap through one stream.
export function rateGate(perSecond) {
    let start = performance.now(), count = 0;
    return () => {
        const now = performance.now();
        if (now - start >= 1000) { start = now; count = 0; }
        return ++count <= perSecond;
    };
}

// Gathers a lookup's records within the limits. Without deduplicateByContent, records count
// as the same only when both id and content match; records without an id are all kept.
export class RecordCollector {
    constructor(limits, deduplicateByContent = false) {
        this.limits = limits; this.byContent = deduplicateByContent;
        this.values = []; this.seen = new Set(); this.total = 0; this.inspected = 0;
    }
    // False once enough records were inspected that the rest is untrusted surplus.
    add(record) {
        if (++this.inspected > this.limits.maxValues * 8) return false;
        if (!record || record.bytes.length > this.limits.maxValueBytes) return true;
        const content = encodeBase64(record.bytes);
        const identity = this.byContent ? `${record.expired}:${content}` :
            record.id ? `${record.id.length}:${record.id}:${content}` : null;
        if (identity && this.seen.has(identity)) return true;
        if (this.values.length >= this.limits.maxValues ||
            record.bytes.length > this.limits.maxTotalBytes - this.total) return true;
        if (identity) this.seen.add(identity);
        this.total += record.bytes.length;
        this.values.push(record);
        return true;
    }
}
