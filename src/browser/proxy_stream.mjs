import { ProxyError } from "./proxy_codec.mjs";

// Each line is bounded before UTF-8 decoding; chunks may split any byte.
// Lines carry untrusted DHT payloads, so an over-long line is skipped up to its
// newline instead of ending the stream.
class LineBuffer {
    constructor(limit, emit, signal) {
        this.bytes = new Uint8Array(limit);
        this.used = 0;
        this.skipping = false;
        this.emit = emit;
        this.signal = signal;
        this.decoder = new TextDecoder("utf-8", { fatal: true });
    }
    push(chunk) {
        if (!(chunk instanceof Uint8Array)) throw new ProxyError("invalid_stream", "expected byte stream");
        let start = 0;
        while (start < chunk.length && !this.signal.aborted) {
            let end = chunk.indexOf(10, start);
            const complete = end >= 0;
            if (!complete) end = chunk.length;
            this.append(chunk.subarray(start, end));
            if (complete) this.flush();
            start = end + 1;
        }
    }
    append(bytes) {
        if (this.skipping) return;
        if (bytes.length > this.bytes.length - this.used) { this.skipping = true; this.used = 0; return; }
        this.bytes.set(bytes, this.used);
        this.used += bytes.length;
    }
    flush() {
        if (this.skipping) { this.skipping = false; return; }
        if (this.used && this.bytes[this.used - 1] === 13) --this.used;
        if (!this.used) throw new ProxyError("invalid_record", "empty proxy record");
        let text;
        try { text = this.decoder.decode(this.bytes.subarray(0, this.used)); }
        catch { throw new ProxyError("invalid_record", "invalid UTF-8 in proxy record"); }
        this.used = 0;
        this.emit(text);
    }
    finish() { if ((this.used || this.skipping) && !this.signal.aborted) this.flush(); }
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
