import { RecoveringMailbox } from "./recovering_mailbox.mjs";

// Matches the WebAssembly advertisement policy and shared C++ window width.
export function discoveryWindows(now) {
    if (!Number.isSafeInteger(now) || now < 0 || now > Number.MAX_SAFE_INTEGER - 300)
        throw new RangeError("invalid discovery clock");
    return { first: Math.floor(Math.max(0, now - 330) / 120), last: Math.floor((now + 30) / 120) };
}

// A single serialized owner coordinates up to four current windows. Each
// stream has its own bounded recovery buffer; records use one caller tracker.
// Catch-up is reported only after all current streams have caught up. Like the
// underlying proxy, that callback does not prove complete network discovery.
export class WindowedMailbox {
    constructor(proxy, discovery, handlers, timers, recovery = {}) {
        if (!proxy || typeof proxy.listen !== "function" || typeof proxy.get !== "function" ||
            typeof discovery?.windowKey !== "function" || typeof handlers?.onRecord !== "function" ||
            typeof handlers.onCatchup !== "function" || typeof handlers.onError !== "function" ||
            typeof handlers.onStatus !== "function" || typeof timers?.set !== "function" ||
            typeof timers.clear !== "function") throw new TypeError("invalid windowed mailbox dependencies");
        const retries = recovery.maxRetries ?? 5, delay = recovery.retryDelayMs ?? 1000;
        if (!Number.isSafeInteger(retries) || retries < 0 || retries > 10 ||
            !Number.isSafeInteger(delay) || delay < 50 || delay > 60000)
            throw new TypeError("invalid discovery recovery limits");
        this.proxy = proxy; this.discovery = discovery; this.handlers = handlers; this.timers = timers;
        this.recovery = recovery; this.entries = new Map(); this.range = null; this.lastNow = null;
        this.started = false; this.closed = false; this.failed = false; this.everReady = false;
        this.availability = "opening"; this.reported = false; this.initial = null;
    }
    start(now) {
        if (this.started || this.closed || this.failed) throw new Error("mailbox cannot be started");
        this.started = true;
        const ready = new Promise((resolve) => { this.initial = resolve; });
        this.tick(now);
        return ready;
    }
    tick(now) {
        if (!this.started || this.closed || this.failed) return false;
        try {
            if (this.lastNow !== null && now < this.lastNow) throw new RangeError("regressing clock");
            const range = discoveryWindows(now); this.lastNow = now;
            if (!this.range || this.range.first !== range.first || this.range.last !== range.last)
                this.reconcile(range);
            return !this.closed && !this.failed;
        } catch (error) { this.fail(error); return false; }
    }
    reconcile(range) {
        this.reported = false;
        for (let window = range.first; window <= range.last; ++window)
            if (!this.entries.has(window)) this.open(window);
        for (const [window, entry] of this.entries) {
            if (window < range.first || window > range.last) { entry.box.close(); this.entries.delete(window); }
        }
        this.range = range;
        this.update();
    }
    open(window) {
        const key = this.discovery.windowKey(window * 120);
        if (!key) throw new Error("cannot derive discovery window key");
        const entry = { box: null, caught: false };
        entry.box = new RecoveringMailbox(this.proxy, key, {
            onRecord: (wire) => { if (this.current(window, entry)) this.handlers.onRecord(wire); },
            onCatchup: () => { if (this.current(window, entry)) { entry.caught = true; this.update(); } },
            onError: (error) => { if (this.current(window, entry)) this.fail(error); },
            onStatus: (status) => this.status(window, entry, status),
        }, this.timers, this.recovery);
        this.entries.set(window, entry);
        void entry.box.start().then((ready) => {
            if (!this.current(window, entry)) return;
            if (!ready && !entry.box.recovering()) this.fail(new Error("discovery window failed to start"));
            else this.update();
        }).catch((error) => { if (this.current(window, entry)) this.fail(error); });
    }
    current(window, entry) { return !this.closed && !this.failed && this.entries.get(window) === entry; }
    status(window, entry, status) {
        if (!this.current(window, entry)) return;
        if (status === "reconnecting") { entry.caught = false; this.reported = false; }
        this.update();
    }
    update() {
        if (this.closed || this.failed || !this.range) return;
        if (!this.healthy()) {
            if (this.everReady && this.availability !== "reconnecting") {
                this.availability = "reconnecting"; this.handlers.onStatus("reconnecting");
            }
            return;
        }
        if (this.everReady && this.availability === "reconnecting") {
            this.availability = "ready"; this.handlers.onStatus("ready");
        }
        if (!this.reported && !this.closed && !this.failed) {
            this.reported = true; this.handlers.onCatchup();
        }
        if (!this.everReady && !this.closed && !this.failed) {
            this.everReady = true; this.availability = "ready"; this.initial?.(true); this.initial = null;
        }
    }
    healthy() {
        return this.started && !this.closed && !this.failed && this.entries.size > 0 &&
            [...this.entries.values()].every((entry) => entry.caught && entry.box.healthy());
    }
    recovering() { return this.everReady && !this.closed && !this.failed && !this.healthy(); }
    fail(error) {
        if (this.closed || this.failed) return;
        this.failed = true; this.shutdown(); this.handlers.onError(error);
    }
    shutdown() {
        for (const entry of this.entries.values()) entry.box.close();
        this.entries.clear(); this.initial?.(false); this.initial = null;
    }
    close() { if (!this.closed) { this.closed = true; this.shutdown(); } }
}
