const bridges = new WeakMap();
const sendResults = ["accepted", "notReady", "tooLarge", "backpressure", "failed", "closed"];
const failures = ["transport", "invalidFrame", "receiveLimit", "allocation"];
const frameLimit = 1024;
const messageLimit = 262145;
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;

function bridgeFor(module) {
    let bridge = bridges.get(module);
    if (bridge) return bridge;
    if (module.p2pdcReliableBridge) throw new TypeError("module already has a reliable bridge");
    const links = new Map();
    bridge = {
        links,
        send: (id, bytes) => links.get(id)?.sendFrame(bytes) ?? 2,
        buffered: (id) => links.get(id)?.buffered() ?? 0,
        message: (id, bytes) => links.get(id)?.enqueue(bytes) ? 1 : 0,
        failure: (id, error) => links.get(id)?.fail(failures[error] ?? "transport"),
        writable: (id) => links.get(id)?.notifyWritable(),
    };
    module.p2pdcReliableBridge = bridge;
    bridges.set(module, bridge);
    return bridge;
}

function validate(module, handlers) {
    if (!module || !module.HEAPU8 || !handlers) throw new TypeError("WebAssembly module and handlers are required");
    for (const name of ["sendFrame", "bufferedAmount", "onMessage", "onFailure", "onWritable"]) {
        if (typeof handlers[name] !== "function") throw new TypeError(`missing ${name} callback`);
    }
    for (const name of ["_malloc", "_free", "_p2pdc_reliable_create", "_p2pdc_reliable_destroy",
                        "_p2pdc_reliable_ready", "_p2pdc_reliable_wake", "_p2pdc_reliable_send",
                        "_p2pdc_reliable_frame"]) {
        if (typeof module[name] !== "function") throw new TypeError(`missing ${name} export`);
    }
}

// Experimental unauthenticated framing bridge. The transport must be ordered,
// reliable and non-reentrant. All handlers are synchronous; application
// notifications are deferred. Close before disposing the WebAssembly module.
export class WasmReliableLink {
    constructor(module, handlers) {
        validate(module, handlers);
        this.module = module;
        this.handlers = handlers;
        this.bridge = bridgeFor(module);
        this.id = module._p2pdc_reliable_create();
        if (!this.id) throw new Error("cannot create WebAssembly reliable link");
        this.closed = false;
        this.failed = false;
        this.pendingBytes = 0;
        this.pendingEvents = 0;
        try { this.bridge.links.set(this.id, this); }
        catch (error) { module._p2pdc_reliable_destroy(this.id); throw error; }
    }
    sendFrame(bytes) {
        if (this.closed || this.failed) return 2;
        try {
            const result = this.handlers.sendFrame(bytes);
            return result === "accepted" ? 0 : result === "notReady" || result === "backpressure" ? 1 : 2;
        } catch {
            console.error("p2pdc: reliable frame callback failed");
            return 2;
        }
    }
    buffered() {
        if (this.closed || this.failed) return 0xffffffff;
        try {
            const value = this.handlers.bufferedAmount();
            if (Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff) return value;
        } catch {
            console.error("p2pdc: reliable buffer callback failed");
        }
        this.fail("callback");
        return 0xffffffff;
    }
    enqueue(bytes) {
        if (this.closed || this.failed || this.pendingEvents >= 128 || bytes.length > 1048576 - this.pendingBytes)
            return false;
        ++this.pendingEvents; this.pendingBytes += bytes.length;
        queueMicrotask(() => {
            --this.pendingEvents; this.pendingBytes -= bytes.length;
            if (this.closed || this.failed) return;
            try { this.handlers.onMessage(bytes); }
            catch { this.fail("callback"); }
        });
        return true;
    }
    notifyWritable() {
        queueMicrotask(() => {
            if (this.closed || this.failed) return;
            try { this.handlers.onWritable(); }
            catch { this.fail("callback"); }
        });
    }
    fail(reason) {
        if (this.closed || this.failed) return;
        this.failed = true;
        queueMicrotask(() => {
            if (this.closed) return;
            this.dispose();
            try { this.handlers.onFailure(reason); }
            catch { console.error("p2pdc: reliable failure callback failed"); }
        });
    }
    dispose() {
        this.bridge.links.delete(this.id);
        this.module._p2pdc_reliable_destroy(this.id);
    }
    close() {
        if (this.closed) return;
        this.closed = true;
        this.dispose();
    }
    setReady(ready, maxMessageSize) {
        if (this.closed || this.failed) return false;
        if (typeof ready !== "boolean") throw new TypeError("ready must be boolean");
        if (ready && (typeof maxMessageSize !== "number" || Number.isNaN(maxMessageSize) ||
                      maxMessageSize < 0 || (maxMessageSize !== 0 && maxMessageSize < frameLimit))) {
            this.fail("transportLimit"); return false;
        }
        this.module._p2pdc_reliable_ready(this.id, ready ? 1 : 0);
        return !this.failed;
    }
    wake() { if (!this.closed && !this.failed) this.module._p2pdc_reliable_wake(this.id); }
    callBytes(name, bytes) {
        let pointer = 0;
        try {
            const owned = new Uint8Array(byteLength.call(bytes));
            Uint8Array.prototype.set.call(owned, bytes);
            pointer = this.module._malloc(Math.max(1, owned.length));
            if (!pointer) { this.fail("allocation"); return null; }
            this.module.HEAPU8.set(owned, pointer);
            return this.module[name](this.id, pointer, owned.length);
        } catch {
            this.fail("allocation");
            return null;
        } finally { if (pointer) this.module._free(pointer); }
    }
    send(bytes) {
        if (this.closed) return "closed";
        if (this.failed) return "failed";
        if (!(bytes instanceof Uint8Array)) throw new TypeError("binary message required");
        if (byteLength.call(bytes) > messageLimit) return "tooLarge";
        const result = this.callBytes("_p2pdc_reliable_send", bytes);
        return result === null ? "failed" : sendResults[result] ?? "failed";
    }
    onFrame(bytes) {
        if (this.closed || this.failed) return false;
        if (!(bytes instanceof Uint8Array)) throw new TypeError("binary frame required");
        if (byteLength.call(bytes) > frameLimit) { this.fail("invalidFrame"); return false; }
        return this.callBytes("_p2pdc_reliable_frame", bytes) !== null && !this.failed;
    }
}
