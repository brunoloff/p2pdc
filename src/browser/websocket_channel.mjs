import { ProxyError } from "./proxy_codec.mjs";

const firstRetryMs = 1000, retryCapMs = 30000;

// Backends reach servers over wss://, or ws:// on this computer for tests.
export function socketUrl(value) {
    const url = new URL(value);
    const local = ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname);
    if ((url.protocol !== "wss:" && !(local && url.protocol === "ws:")) || url.username || url.password || url.hash)
        throw new TypeError("server must be a wss:// URL without credentials or fragment");
    return url;
}

// One WebSocket to a server, opened when first needed and opened again after it closes, no
// sooner than a backoff that doubles after each failed attempt. Owners learn of messages and
// closes through handlers; whatever was in flight when the socket closed has failed.
export class SocketChannel {
    constructor(url, handlers, options = {}) {
        this.url = socketUrl(url).href;
        this.handlers = handlers; this.protocols = options.protocols;
        this.timeoutMs = options.timeoutMs ?? 10000;
        this.Socket = options.WebSocket ?? globalThis.WebSocket;
        if (typeof this.Socket !== "function") throw new TypeError("WebSocket is required");
        this.socket = null; this.opening = null; this.closed = false;
        this.retryMs = firstRetryMs; this.notBefore = 0;
    }
    get open() { return this.socket?.readyState === 1; }
    // Resolves once the socket is open; rejects with a transient ProxyError when it cannot be.
    ready() {
        if (this.closed) return Promise.reject(new ProxyError("network_error", "channel closed"));
        if (this.open) return Promise.resolve();
        if (this.opening) return this.opening;
        if (Date.now() < this.notBefore)
            return Promise.reject(new ProxyError("network_error", "server unreachable; waiting to try again"));
        this.opening = this.connect().finally(() => { this.opening = null; });
        return this.opening;
    }
    connect() {
        return new Promise((resolve, reject) => {
            let settled = false;
            const socket = new this.Socket(this.url, this.protocols);
            socket.binaryType = "arraybuffer";
            const fail = (error) => {
                if (settled) return;
                settled = true; clearTimeout(timer);
                this.notBefore = Date.now() + this.retryMs;
                this.retryMs = Math.min(this.retryMs * 2, retryCapMs);
                try { socket.close(); } catch { /* already closing */ }
                reject(error);
            };
            const timer = setTimeout(() => fail(new ProxyError("timeout", "server did not answer")), this.timeoutMs);
            socket.onopen = () => {
                if (settled) return;
                settled = true; clearTimeout(timer);
                this.socket = socket; this.retryMs = firstRetryMs;
                resolve();
            };
            socket.onerror = () => fail(new ProxyError("network_error", "server connection failed"));
            socket.onmessage = (event) => { if (this.socket === socket) this.handlers.message(event.data); };
            socket.onclose = () => {
                fail(new ProxyError("network_error", "server closed the connection"));
                if (this.socket !== socket) return;
                this.socket = null;
                this.handlers.close(new ProxyError("stream_closed", "server connection closed"));
            };
        });
    }
    send(data) {
        if (!this.open) throw new ProxyError("network_error", "server connection is not open");
        this.socket.send(data);
    }
    close() {
        this.closed = true;
        const socket = this.socket;
        this.socket = null;
        if (socket) { try { socket.close(); } catch { /* already closing */ } }
    }
}

// A promise for one reply, settled by its key, rejected on timeout, abort or a closed channel.
export class PendingReplies {
    constructor(timeoutMs) { this.timeoutMs = timeoutMs; this.waiting = new Map(); }
    wait(id, signal, timeoutMs = this.timeoutMs) {
        return new Promise((resolve, reject) => {
            if (signal?.aborted) { reject(new DOMException("request aborted", "AbortError")); return; }
            const abort = () => this.fail(id, new DOMException("request aborted", "AbortError"));
            const timer = setTimeout(() => this.fail(id, new ProxyError("timeout", "server did not reply")), timeoutMs);
            signal?.addEventListener("abort", abort, { once: true });
            this.waiting.set(id, { resolve, reject, done: () => { clearTimeout(timer); signal?.removeEventListener("abort", abort); } });
        });
    }
    has(id) { return this.waiting.has(id); }
    settle(id, value) {
        const entry = this.waiting.get(id);
        if (!entry) return false;
        this.waiting.delete(id); entry.done(); entry.resolve(value);
        return true;
    }
    fail(id, error) {
        const entry = this.waiting.get(id);
        if (!entry) return;
        this.waiting.delete(id); entry.done(); entry.reject(error);
    }
    failAll(error) { for (const id of [...this.waiting.keys()]) this.fail(id, error); }
}
