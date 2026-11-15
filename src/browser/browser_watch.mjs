import { WasmDiscovery } from "./wasm_discovery.mjs";
import { RecoveringMailbox } from "./recovering_mailbox.mjs";
import { WindowedMailbox } from "./windowed_mailbox.mjs";
import { timing } from "./discovery_clock.mjs";

// One best-effort verified group watch. Advertisements identify keys, not
// authorization. An update invalidates the old handle. The owner rechecks
// current() immediately before use and supplies application consent separately.
export class BrowserWatch {
    constructor(module, proxy, identity, handlers, options = {}) {
        if (!handlers || typeof handlers.onPeer !== "function" || typeof handlers.onCatchup !== "function" ||
            typeof handlers.onError !== "function" || (handlers.onStatus != null && typeof handlers.onStatus !== "function"))
            throw new TypeError("invalid watch callbacks");
        this.handlers = handlers;
        this.windowed = options.windowed ?? true;
        if (typeof this.windowed !== "boolean") throw new TypeError("invalid discovery mode");
        this.interval = options.sweepIntervalMs ?? 500;
        if (!Number.isSafeInteger(this.interval) || this.interval < 50 || this.interval > 60000)
            throw new TypeError("invalid sweep interval");
        ({ timers: this.timers, clock: this.clock } = timing(options));
        this.closed = false; this.failed = false; this.started = false; this.timer = null;
        this.discovery = new WasmDiscovery(module, identity, {
            onPeer: (peer, event) => this.peer(peer, event), onError: (reason) => this.fail(reason),
        });
        try {
            const mailboxHandlers = {
                onRecord: (wire) => this.record(wire), onCatchup: () => this.catchup(),
                onError: () => this.fail("backend"), onStatus: (status) => this.status(status),
            };
            if (this.windowed) this.mailbox = new WindowedMailbox(proxy, this.discovery, mailboxHandlers,
                this.timers, options.recovery);
            else {
                const key = this.discovery.key();
                if (!key) throw new Error("cannot derive group key");
                this.mailbox = new RecoveringMailbox(proxy, key, mailboxHandlers, this.timers, options.recovery);
            }
        } catch (error) { this.discovery.close(); throw error; }
    }
    async start() {
        if (this.started || this.closed || this.failed) throw new Error("watch cannot be started");
        this.started = true;
        this.schedule();
        if (this.closed || this.failed) return false;
        let started;
        try { started = await this.mailbox.start(this.windowed ? this.clock.read() : undefined); }
        catch { this.fail("clock"); return false; }
        if (this.closed || this.failed) return false;
        if (!started) { this.fail("backend"); return false; }
        return true;
    }
    record(wire) {
        if (this.closed || this.failed) return;
        try {
            const result = this.discovery.receive(wire, this.clock.read());
            if (result === "failed" || result === "closed") this.fail("discovery");
        } catch { this.fail("clock"); }
    }
    peer(peer, event) {
        if (this.closed || this.failed) return;
        try { this.handlers.onPeer(peer, event); }
        catch { this.fail("callback"); }
    }
    catchup() {
        if (this.closed || this.failed) return;
        try { this.handlers.onCatchup(); }
        catch { this.fail("callback"); }
    }
    status(status) {
        if (this.closed || this.failed) return;
        try { this.handlers.onStatus?.(status); }
        catch { this.fail("callback"); }
    }
    schedule() {
        if (this.closed || this.failed || this.timer !== null) return;
        try {
            const timer = this.timers.set(() => { this.timer = null; this.tick(); }, this.interval);
            if (timer == null) throw new Error("timer returned no handle");
            this.timer = timer;
        }
        catch { this.fail("scheduler"); }
    }
    tick() {
        if (this.closed || this.failed) return;
        try {
            const now = this.clock.read();
            if (!this.discovery.sweep(now)) { this.fail("discovery"); return; }
            if (this.windowed && !this.mailbox.tick(now)) return;
        }
        catch { this.fail("clock"); return; }
        this.schedule();
    }
    current(handle) {
        if (this.closed || this.failed) return null;
        if (!Number.isSafeInteger(handle) || handle <= 0 || handle > 0xffffffff) throw new TypeError("invalid peer handle");
        if (!this.mailbox.healthy()) return null;
        try { return this.discovery.current(handle, this.clock.read()); }
        catch { this.fail("clock"); return null; }
    }
    healthy() { return this.started && !this.closed && !this.failed && this.mailbox.healthy(); }
    recovering() {
        return !this.closed && !this.failed && this.mailbox.recovering();
    }
    fail(reason) {
        if (this.closed || this.failed) return;
        this.failed = true; this.stop();
        queueMicrotask(() => {
            if (this.closed) return;
            try { this.handlers.onError(reason); }
            catch { console.error("p2pdc: watch error callback failed"); }
        });
    }
    stop() {
        if (this.timer !== null) {
            const timer = this.timer; this.timer = null;
            try { this.timers.clear(timer); } catch (error) { console.error("p2pdc: watch timer cancellation failed", error); }
        }
        this.mailbox.close(); this.discovery.close();
    }
    close() { if (!this.closed) { this.closed = true; this.stop(); } }
}
