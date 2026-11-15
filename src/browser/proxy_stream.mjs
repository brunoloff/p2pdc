import { ProxyError } from "./proxy_codec.mjs";

// Each line is bounded before UTF-8 decoding; chunks may split any byte.
class LineBuffer {
    constructor(limit, emit, signal) {
        this.bytes = new Uint8Array(limit);
        this.used = 0;
        this.emit = emit;
        this.signal = signal;
        this.decoder = new TextDecoder("utf-8", { fatal: true });
    }
    push(chunk) {
        if (!(chunk instanceof Uint8Array)) throw new ProxyError("invalid_stream", "expected byte stream");
        let start = 0;
        while (start < chunk.length && !this.signal.aborted) {
            let end = chunk.indexOf(10, start);
            if (end < 0) end = chunk.length;
            const size = end - start;
            if (size > this.bytes.length - this.used) throw new ProxyError("resource_limit", "proxy line too large");
            this.bytes.set(chunk.subarray(start, end), this.used);
            this.used += size;
            if (end < chunk.length) this.flush();
            start = end + 1;
        }
    }
    flush() {
        if (this.used && this.bytes[this.used - 1] === 13) --this.used;
        if (!this.used) throw new ProxyError("invalid_record", "empty proxy record");
        let text;
        try { text = this.decoder.decode(this.bytes.subarray(0, this.used)); }
        catch { throw new ProxyError("invalid_record", "invalid UTF-8 in proxy record"); }
        this.used = 0;
        this.emit(text);
    }
    finish() { if (this.used && !this.signal.aborted) this.flush(); }
}

export async function readLines(body, signal, maxLineBytes, emit) {
    if (!body || typeof body.getReader !== "function") throw new ProxyError("invalid_stream", "missing proxy response stream");
    const reader = body.getReader();
    const lines = new LineBuffer(maxLineBytes, emit, signal);
    let cancellation, cancellationError;
    const cancel = () => { cancellation ??= reader.cancel(); return cancellation; };
    const onAbort = () => { void cancel().catch((error) => { cancellationError = error; }); };
    signal.addEventListener("abort", onAbort, { once: true });
    try {
        while (!signal.aborted) {
            let result;
            try { result = await reader.read(); }
            catch (error) {
                if (signal.aborted || error?.name === "AbortError") throw error;
                throw new ProxyError("network_error", "proxy stream read failed");
            }
            const { value, done } = result;
            if (signal.aborted) break;
            if (done) { lines.finish(); return; }
            lines.push(value);
        }
    } finally {
        signal.removeEventListener("abort", onAbort);
        try { await cancel(); } catch (error) { cancellationError = error; }
        if (cancellationError && cancellationError.name !== "AbortError")
            console.error("p2pdc: proxy stream cleanup failed", cancellationError);
        reader.releaseLock();
    }
}
