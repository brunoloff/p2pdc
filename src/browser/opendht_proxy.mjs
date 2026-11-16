import { ProxyError, decodeRecordOrDrop, encodeBase64, keyHex } from "./proxy_codec.mjs";
import { readLines } from "./proxy_stream.mjs";
export { ProxyError } from "./proxy_codec.mjs";

function proxyBase(value) {
    const url = new URL(value);
    const local = ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname);
    if ((url.protocol !== "https:" && !(local && url.protocol === "http:")) ||
        url.username || url.password || url.search || url.hash) {
        throw new TypeError("proxy must be an HTTPS URL without credentials, query, or fragment");
    }
    url.pathname = `${url.pathname.replace(/\/+$/, "")}/`;
    return url;
}
function bounds(options) {
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
function requestScope(parent, timeoutMs) {
    const controller = new AbortController();
    const abort = () => controller.abort(parent?.reason);
    if (parent?.aborted) abort();
    else parent?.addEventListener("abort", abort, { once: true });
    let timedOut = false;
    let timer = setTimeout(() => { timedOut = true; controller.abort(); }, timeoutMs);
    const disarm = () => { if (timer !== null) clearTimeout(timer); timer = null; };
    return { signal: controller.signal, close: () => controller.abort(), timedOut: () => timedOut,
             disarm, dispose: () => { disarm(); parent?.removeEventListener("abort", abort); } };
}
function checkResponse(response, scope) {
    if (!response.ok) {
        scope.close();
        if (response.body) void response.body.cancel().catch((error) => {
            console.error("p2pdc: failed to cancel proxy error response", error);
        });
        const error = new ProxyError("http_error", `proxy returned HTTP ${response.status}`);
        error.status = response.status;
        throw error;
    }
    return response;
}
function lineLimit(maxValueBytes) { return 4 * Math.ceil(maxValueBytes / 3) + 4096; }
// Admits at most perSecond records per one-second window; the rest are dropped
// so a hostile proxy cannot flood the WebAssembly heap through one stream.
function rateGate(perSecond) {
    let start = performance.now(), count = 0;
    return () => {
        const now = performance.now();
        if (now - start >= 1000) { start = now; count = 0; }
        return ++count <= perSecond;
    };
}
function requestError(error, scope) {
    return scope.timedOut() ? new ProxyError("timeout", "proxy request timed out") : error;
}
async function cancelResponse(body, signal) {
    if (!body) return;
    if (signal.aborted) throw new DOMException("request aborted", "AbortError");
    let abort;
    const canceled = new Promise((_resolve, reject) => {
        abort = () => reject(new DOMException("request aborted", "AbortError"));
        signal.addEventListener("abort", abort, { once: true });
    });
    try { await Promise.race([body.cancel(), canceled]); }
    finally { signal.removeEventListener("abort", abort); }
}

// Uses only caller-supplied proxy URLs. Values are opaque and must be verified
// by the C++ security layer. HTTP success does not establish DHT completeness.
export class OpenDhtProxy {
    constructor(url, options = {}) {
        this.base = proxyBase(url);
        this.limits = bounds(options);
        const fetcher = options.fetch ?? globalThis.fetch;
        if (typeof fetcher !== "function") throw new TypeError("fetch is required");
        this.fetch = options.fetch == null ? fetcher.bind(globalThis) : fetcher;
    }
    url(key, listen = false) {
        return new URL(`key/${keyHex(key)}${listen ? "/listen" : ""}`, this.base).href;
    }
    async request(url, init, scope) {
        try {
            return checkResponse(await this.fetch(url, { ...init, signal: scope.signal, mode: "cors",
                                                        credentials: "omit", redirect: "error", cache: "no-store" }), scope);
        } catch (error) {
            const failure = requestError(error, scope);
            if (failure instanceof ProxyError || failure?.name === "AbortError") throw failure;
            throw new ProxyError("network_error", "proxy network request failed");
        }
    }
    async put(key, value, signal) {
        if (!(value instanceof Uint8Array) || value.length > this.limits.maxValueBytes ||
            value.length > this.limits.maxTotalBytes) throw new TypeError("invalid or oversized payload");
        const url = this.url(key);
        const scope = requestScope(signal, this.limits.timeoutMs);
        try {
            // The OpenDHT proxy accepts JSON as text but does not implement
            // OPTIONS; a safelisted content type permits cross-origin POSTs.
            const response = await this.request(url, { method: "POST", headers: { "Content-Type": "text/plain;charset=UTF-8" },
                                                       body: JSON.stringify({ data: encodeBase64(value) }) }, scope);
            await cancelResponse(response.body, scope.signal);
        } catch (error) { throw requestError(error, scope); }
        finally { scope.dispose(); }
    }
    async get(key, signal, options = {}) {
        if (!options || typeof options !== "object" || Array.isArray(options) ||
            (typeof options.deduplicateByContent !== "undefined" && typeof options.deduplicateByContent !== "boolean"))
            throw new TypeError("invalid lookup deduplication option");
        const url = this.url(key);
        const scope = requestScope(signal, this.limits.timeoutMs);
        const values = [], seen = new Set();
        let total = 0, inspected = 0;
        try {
            const response = await this.request(url, { method: "GET" }, scope);
            if (response.body) await readLines(response.body, scope.signal, lineLimit(this.limits.maxValueBytes), (line) => {
                // Records beyond the limits are untrusted surplus: drop them.
                if (options.deduplicateByContent && ++inspected > this.limits.maxValues * 8) return;
                const record = decodeRecordOrDrop(line, this.limits.maxValueBytes);
                if (!record) return;
                const content = encodeBase64(record.bytes);
                const identity = options.deduplicateByContent ? `${record.expired}:${content}` :
                    record.id && record.id !== "0" ? `${record.id.length}:${record.id}:${content}` : null;
                if (identity && seen.has(identity)) return;
                if (values.length >= this.limits.maxValues || record.bytes.length > this.limits.maxTotalBytes - total)
                    return;
                if (identity) seen.add(identity);
                total += record.bytes.length;
                values.push(record);
            });
            if (scope.signal.aborted) throw new DOMException("request aborted", "AbortError");
            return values;
        } catch (error) {
            throw requestError(error, scope);
        } finally { scope.dispose(); }
    }
    listen(key, onRecord, signal) {
        if (typeof onRecord !== "function") throw new TypeError("record callback is required");
        const url = this.url(key, true);
        const state = { closed: false, scope: null, resolveReady: null };
        const ready = new Promise((resolve) => { state.resolveReady = resolve; });
        const done = (async () => {
            try { while (await this.listenOnce(url, onRecord, signal, state)); }
            finally { state.resolveReady(false); }
        })();
        return { ready, done, close: () => { state.closed = true; state.scope?.close(); } };
    }
    // One listen request. The proxy sends no keep-alives, so a stalled
    // connection looks like a quiet key: after listenIdleMs without a record the
    // stream is restarted transparently (resolves true). Re-sent records are
    // deduplicated by the verifying layers above.
    async listenOnce(url, onRecord, signal, state) {
        const scope = requestScope(signal, this.limits.timeoutMs);
        state.scope = scope;
        if (state.closed) scope.close();
        let idle = false, timer = null;
        const touch = () => {
            clearTimeout(timer);
            timer = setTimeout(() => { idle = true; scope.close(); }, this.limits.listenIdleMs);
        };
        const admit = rateGate(this.limits.maxRecordsPerSecond);
        try {
            const response = await this.request(url, { method: "GET" }, scope);
            scope.disarm();
            if (scope.timedOut()) throw new ProxyError("timeout", "proxy request timed out");
            state.resolveReady(!scope.signal.aborted);
            touch();
            await readLines(response.body, scope.signal, lineLimit(this.limits.maxValueBytes), (line) => {
                if (scope.signal.aborted) return;
                touch();
                const record = decodeRecordOrDrop(line, this.limits.maxValueBytes);
                if (record && record.bytes.length <= this.limits.maxTotalBytes && admit()) onRecord(record);
            });
            if (!scope.signal.aborted) throw new ProxyError("stream_closed", "proxy listen stream ended");
            if (idle && !state.closed) return true;
            if (!state.closed) throw new DOMException("request aborted", "AbortError");
            return false;
        } catch (error) {
            if (idle && !state.closed && error?.name === "AbortError") return true;
            if (!(state.closed && scope.signal.aborted && error?.name === "AbortError")) throw error;
            return false;
        } finally { clearTimeout(timer); scope.dispose(); }
    }
}
