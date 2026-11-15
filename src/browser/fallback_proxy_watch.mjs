import { PollingProxyWatch } from "./polling_proxy_watch.mjs";

/** @param {unknown} error */
function isTimeout(error) {
    return typeof error === "object" && error !== null && "code" in error && error.code === "timeout";
}

// A quiet proxy stream may not expose its headers to every browser. Fall back
// on a startup deadline; later stream failures remain visible to the caller.
export class FallbackProxyWatch {
    constructor(proxy, key, emit, cache, interval, signal, startupMs = 2000) {
        this.closed = false; this.poll = null;
        this.stream = proxy.listen(key, emit, signal);
        this.ready = new Promise((resolve) => { this.resolveReady = resolve; });
        this.stopped = new Promise((resolve) => { this.resolveStopped = resolve; });
        this.done = this.run(proxy, key, emit, cache, interval, startupMs);
    }
    async run(proxy, key, emit, cache, interval, startupMs) {
        /** @type {{ended?: boolean, error?: unknown} | undefined} */
        let settled;
        const completion = this.stream.done.then(() => (settled = { ended: true }), (error) => (settled = { error }));
        let timer;
        const deadline = new Promise((resolve) => { timer = setTimeout(() => resolve({ timeout: true }), startupMs); });
        try {
            const first = await Promise.race([this.stream.ready.then((ready) => ({ ready })), completion,
                                              deadline, this.stopped]);
            clearTimeout(timer);
            if (this.closed) return;
            if (first.ready === false) await Promise.resolve();
            const failure = first.ready === false && settled && "error" in settled ? settled : first;
            if (first.timeout || ("error" in failure && isTimeout(failure.error))) {
                this.stream.close();
                this.poll = new PollingProxyWatch(proxy, key, emit, cache, interval);
                this.resolveReady(await this.poll.ready);
                await this.poll.done;
            } else {
                if ("error" in failure) throw failure.error;
                if ("ready" in first) this.resolveReady(first.ready);
                if (!first.ended) {
                    const result = await Promise.race([completion, this.stopped]);
                    if (result && "error" in result && !this.closed) throw result.error;
                }
            }
        } finally { clearTimeout(timer); this.resolveReady(false); }
    }
    close() {
        if (this.closed) return;
        this.closed = true;
        this.resolveStopped({ stopped: true });
        this.stream.close();
        this.poll?.close();
    }
}
