import { BrowserWatch } from "./browser_watch.mjs";
import { BrowserOutgoing } from "./browser_outgoing.mjs";
import { BrowserIncoming } from "./browser_incoming.mjs";
import { copyIdentity, sameBytes, sameEndpoint, wipeIdentity } from "./browser_owner_utils.mjs";

const samePeer = (a, b) => sameBytes(a.signingKey, b.signingKey) && sameBytes(a.instance, b.instance);

// One discovery capability and identity. Callers explicitly authorize peers;
// an incoming listener never accepts a request automatically.
export class BrowserNode {
    constructor(module, proxy, identity, handlers, options = {}) {
        for (const name of ["onPeer", "onCatchup", "onError"]) {
            if (typeof handlers?.[name] !== "function") throw new TypeError(`missing ${name} callback`);
        }
        if (handlers.onStatus != null && typeof handlers.onStatus !== "function") throw new TypeError("invalid status callback");
        this.identity = copyIdentity(identity);
        this.module = module; this.proxy = proxy; this.handlers = handlers; this.state = "new"; this.epoch = 0;
        this.handles = new Set(); this.peers = new WeakSet(); this.pendingEvents = []; this.deferredPeers = [];
        try {
            const watchOptions = { ...options.watch, windowed: options.watch?.windowed ?? true };
            this.watch = new BrowserWatch(module, proxy, this.identity, {
                onPeer: (peer, event) => this.peer(peer, event), onCatchup: () => this.catchup(),
                onError: (reason) => this.fail(reason),
                onStatus: (status) => { if (status === "ready") this.retryListeners(); this.discoveryEvent("onStatus", status);
                    if (status === "ready") this.flushPeers(); },
            }, watchOptions);
        } catch (error) { wipeIdentity(this.identity); this.identity = null; throw error; }
    }
    async start() {
        if (this.state !== "new") throw new Error("node cannot be started");
        this.state = "starting";
        try {
            const started = await this.watch.start();
            if (this.state !== "starting") return false;
            if (!started) { this.fail("discovery"); return false; }
            this.state = "running";
            for (const [name, values] of this.pendingEvents) this.notify(name, ...values);
            this.pendingEvents.length = 0;
            return true;
        } catch { this.fail("discovery"); return false; }
    }
    peer(peer, event) {
        if (!["starting", "running"].includes(this.state)) return;
        if (event !== "expired") this.peers.add(peer);
        this.retryListeners(); this.discoveryEvent("onPeer", peer, event);
    }
    catchup() {
        if (!["starting", "running"].includes(this.state)) return;
        this.retryListeners(); this.discoveryEvent("onCatchup"); this.flushPeers();
    }
    discoveryEvent(name, ...values) {
        if (this.state === "starting") {
            if (this.pendingEvents.length >= 1024) { this.fail("resource"); return; }
            this.pendingEvents.push([name, values]);
        } else this.notify(name, ...values);
    }
    retryListeners() {
        for (const handle of this.handles) if (handle instanceof BrowserIncoming) handle.retry();
    }
    notify(name, ...values) {
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (epoch !== this.epoch || ["closed", "failed"].includes(this.state)) return;
            try {
                if (name === "onPeer" && values[1] !== "expired") {
                    const current = this.watch.current(values[0].handle);
                    if (!current && this.watch.recovering()) { this.deferPeer(values[0], values[1]); return; }
                    if (!current || !sameEndpoint(current, values[0])) return;
                }
                this.handlers[name]?.(...values);
            } catch { this.fail("callback"); }
        });
    }
    deferPeer(peer, event) {
        const index = this.deferredPeers.findIndex(([previous]) => samePeer(previous, peer));
        if (index >= 0) { this.deferredPeers[index] = [peer, event]; return; }
        if (this.deferredPeers.length >= 128) { this.fail("resource"); return; }
        this.deferredPeers.push([peer, event]);
    }
    flushPeers() {
        if (this.state !== "running" || !this.watch.healthy()) return;
        const peers = this.deferredPeers; this.deferredPeers = [];
        for (const [peer, event] of peers) this.notify("onPeer", peer, event);
    }
    reserve() {
        if (this.state !== "running" || !this.watch.healthy()) throw new Error("node is not ready");
        for (const handle of this.handles) if (["closed", "failed"].includes(handle.state)) this.handles.delete(handle);
        if (this.handles.size >= 8) throw new RangeError("connection limit reached");
    }
    operationOptions(options, field) {
        const inner = options?.[field] ?? {};
        if (typeof inner !== "object" || Array.isArray(inner) ||
            (inner.windowed !== undefined && inner.windowed !== this.watch.windowed))
            throw new TypeError("announcement and watch discovery protocols must match");
        return { ...options, [field]: { ...inner, windowed: this.watch.windowed } };
    }
    connect(peer, options, handlers) {
        this.reserve();
        if (!peer || !this.peers.has(peer)) throw new TypeError("peer does not belong to this node");
        const current = this.watch.current(peer.handle);
        if (!current || !sameEndpoint(current, peer)) throw new Error("peer is stale");
        const configured = this.operationOptions(options, "lease");
        const handle = new BrowserOutgoing(this.module, this.proxy, this.identity, this.watch, peer, configured, handlers);
        this.handles.add(handle); this.launch(handle); return handle;
    }
    listen(options, handlers) {
        this.reserve();
        const configured = this.operationOptions(options, "listener");
        const handle = new BrowserIncoming(this.module, this.proxy, this.identity, this.watch, configured, handlers);
        this.handles.add(handle); this.launch(handle); return handle;
    }
    launch(handle) {
        void handle.start().catch(() => handle.fail("startup"));
    }
    fail(reason) {
        if (["closed", "failed"].includes(this.state)) return;
        this.state = "failed"; ++this.epoch; this.watch.close(); this.pendingEvents.length = 0; this.deferredPeers.length = 0;
        wipeIdentity(this.identity); this.identity = null;
        for (const handle of this.handles) if (!handle.authenticated) handle.close();
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (this.state !== "failed" || epoch !== this.epoch) return;
            try { this.handlers.onError(reason); } catch (error) { console.error("p2pdc: node error callback failed", error); }
        });
    }
    close() {
        if (this.state === "closed") return;
        this.state = "closed"; ++this.epoch; this.watch.close(); this.pendingEvents.length = 0; this.deferredPeers.length = 0;
        for (const handle of this.handles) handle.close();
        this.handles.clear(); wipeIdentity(this.identity); this.identity = null;
    }
}
