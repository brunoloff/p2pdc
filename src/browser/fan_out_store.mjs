import { ProxyError } from "./proxy_codec.mjs";
import { RecordCollector } from "./store_limits.mjs";

// After the first backend answers a lookup, how long the others get before the merged answer
// goes out without them: a blocked backend must not hold discovery up.
const lookupGraceMs = 1500;
// A failed watch on one backend is started again after a backoff, doubling up to the cap.
const rewatchMs = 2000, rewatchCapMs = 60000;

function allFailed(errors) {
    return errors.find((error) => error?.name !== "AbortError") ?? errors[0] ??
        new ProxyError("network_error", "every discovery backend failed");
}

// Several separate meeting places used at once (see docs/DISCOVERY_BACKENDS.md): puts go to
// all of them, lookups merge their answers and watches watch all of them, so two nodes meet
// through any backend both can reach.
export class FanOutStore {
    constructor(backends, limits) {
        if (!Array.isArray(backends) || backends.length < 2) throw new TypeError("fan-out needs two or more backends");
        this.backends = backends.slice();
        this.limits = limits;
        // Streaming HTTP backends count against their origin's connection budget.
        this.streamOrigin = backends.find((backend) => backend.streamOrigin)?.streamOrigin ?? null;
    }
    close() { for (const backend of this.backends) backend.close?.(); }
    put(key, value, signal) {
        return Promise.any(this.backends.map((backend) => backend.put(key, value, signal)))
            .catch((error) => { throw allFailed(error.errors); });
    }
    async get(key, signal, options = {}) {
        const answers = this.backends.map(() => null), errors = [];
        let settled = 0, grace = null, finish;
        const done = new Promise((resolve) => { finish = resolve; });
        this.backends.forEach((backend, index) => {
            backend.get(key, signal, options).then((records) => { answers[index] = records; },
                                                   (error) => { errors.push(error); })
                .finally(() => {
                    if (++settled === this.backends.length) finish();
                    else if (answers[index] && grace === null) grace = setTimeout(finish, lookupGraceMs);
                });
        });
        await done;
        clearTimeout(grace);
        if (!answers.some(Boolean)) throw allFailed(errors);
        return this.merge(answers, options.deduplicateByContent === true);
    }
    merge(answers, byContent) {
        const collector = new RecordCollector(this.limits, byContent);
        answers.forEach((records, index) => {
            for (const record of records ?? [])
                collector.add({ ...record, id: record.id ? `${index}:${record.id}` : record.id });
        });
        return collector.values;
    }
    listen(key, onRecord, signal) {
        const watch = new FanOutWatch(this.backends, key, onRecord, signal);
        return { ready: watch.ready, done: watch.done, close: () => watch.close() };
    }
}

// Watches every backend. Ready once one is; one that fails is watched again after a backoff
// while it is not the last one standing. Done when closed, or when every backend has failed
// before any became ready.
class FanOutWatch {
    constructor(backends, key, onRecord, signal) {
        this.key = key; this.onRecord = onRecord; this.signal = signal; this.closed = false;
        this.total = backends.length;
        this.subscriptions = new Map(); this.timers = new Set(); this.everReady = false;
        this.failures = new Map();
        this.ready = new Promise((resolve) => { this.resolveReady = resolve; });
        this.done = new Promise((resolve, reject) => { this.resolveDone = resolve; this.rejectDone = reject; });
        signal?.addEventListener("abort", () => this.close(), { once: true });
        backends.forEach((backend, index) => this.start(backend, index, rewatchMs));
    }
    start(backend, index, backoff) {
        if (this.closed) return;
        let subscription;
        try {
            subscription = backend.listen(this.key, (record) => {
                if (!this.closed) this.onRecord({ ...record, id: record.id ? `${index}:${record.id}` : record.id });
            }, this.signal);
        } catch (error) { this.failed(backend, index, backoff, error); return; }
        this.subscriptions.set(index, subscription);
        subscription.ready.then((ready) => {
            if (!ready || this.closed) return;
            this.everReady = true; this.failures.delete(index); this.resolveReady(true);
        });
        subscription.done.then(() => this.ended(backend, index, backoff, null),
                               (error) => this.ended(backend, index, backoff, error));
    }
    ended(backend, index, backoff, error) {
        this.subscriptions.delete(index);
        if (!this.closed) this.failed(backend, index, backoff, error);
    }
    failed(backend, index, backoff, error) {
        this.failures.set(index, error);
        if (!this.everReady && this.failures.size === this.total) {
            this.stop();
            this.resolveReady(false);
            this.rejectDone(error ?? new ProxyError("stream_closed", "every discovery watch ended"));
            return;
        }
        const timer = setTimeout(() => {
            this.timers.delete(timer);
            this.start(backend, index, Math.min(backoff * 2, rewatchCapMs));
        }, backoff);
        this.timers.add(timer);
    }
    stop() {
        this.closed = true;
        for (const timer of this.timers) clearTimeout(timer);
        this.timers.clear();
        for (const subscription of this.subscriptions.values()) subscription.close();
        this.subscriptions.clear();
    }
    close() {
        if (this.closed) return;
        this.stop();
        this.resolveReady(false);
        this.resolveDone();
    }
}
