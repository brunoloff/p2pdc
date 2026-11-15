const modules = new WeakMap();
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;
const events = ["seen", "updated", "expired"];

function copied(value, size) {
    if (!(value instanceof Uint8Array) || byteLength.call(value) !== size) throw new TypeError("invalid binary input");
    const result = new Uint8Array(size); Uint8Array.prototype.set.call(result, value); return result;
}
function validate(module, handlers) {
    if (!module?.HEAPU8 || !handlers || typeof handlers.onPeer !== "function" || typeof handlers.onError !== "function")
        throw new TypeError("invalid discovery dependencies");
    for (const name of ["_malloc", "_free", "_p2pdc_discovery_create", "_p2pdc_discovery_destroy", "_p2pdc_discovery_healthy",
                        "_p2pdc_discovery_key", "_p2pdc_discovery_self", "_p2pdc_discovery_publish", "_p2pdc_discovery_receive",
                        "_p2pdc_discovery_sweep", "_p2pdc_discovery_current"])
        if (typeof module[name] !== "function") throw new TypeError(`missing ${name} export`);
}
function bridgeFor(module) {
    let bridge = modules.get(module);
    if (bridge) return bridge;
    if (module.p2pdcDiscoveryBridge) throw new TypeError("module already has a discovery bridge");
    const watchers = new Map();
    bridge = { watchers, event: (id, handle, kind, bytes) => watchers.get(id)?.enqueue(handle, kind, bytes) ?? false };
    module.p2pdcDiscoveryBridge = bridge; modules.set(module, bridge);
    return bridge;
}
function unpack(bytes, handle) {
    if (!(bytes instanceof Uint8Array) || bytes.length !== 132) throw new TypeError("invalid peer record");
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return { handle, signingKey: bytes.slice(0, 32), recipientKey: bytes.slice(32, 64),
        instance: bytes.slice(64, 80), inbox: bytes.slice(80, 100), sequence: view.getBigUint64(100),
        features: view.getBigUint64(108), issuedAt: view.getBigUint64(116), expiresAt: view.getBigUint64(124) };
}
function configBytes(options) {
    if (!options) throw new TypeError("discovery options required");
    const result = new Uint8Array(96);
    try {
        for (const [index, name] of ["identitySeed", "recipientSecret", "capability"].entries()) {
            const value = copied(options[name], 32);
            try { result.set(value, index * 32); } finally { value.fill(0); }
        }
        return result;
    } catch (error) { result.fill(0); throw error; }
}

// Internal discovery codec and tracker. Verified advertisements identify keys;
// the application still authorizes peers and rechecks current() before connecting.
// Updated peers get a new handle; replace prior entries by signingKey + instance.
export class WasmDiscovery {
    constructor(module, options, handlers) {
        validate(module, handlers);
        this.module = module; this.handlers = handlers; this.bridge = bridgeFor(module);
        this.closed = false; this.failed = false; this.pending = 0;
        const config = configBytes(options);
        try { this.id = this.create(config); } finally { config.fill(0); }
        if (!this.id) throw new Error("cannot create discovery tracker");
        try { this.bridge.watchers.set(this.id, this); }
        catch (error) { module._p2pdc_discovery_destroy(this.id); throw error; }
    }
    create(config) {
        const pointer = this.module._malloc(config.length);
        if (!pointer) throw new Error("allocation failed");
        try { this.module.HEAPU8.set(config, pointer); return this.module._p2pdc_discovery_create(pointer, config.length); }
        finally { this.module.HEAPU8.fill(0, pointer, pointer + config.length); this.module._free(pointer); }
    }
    enqueue(handle, kind, bytes) {
        if (this.closed || this.failed || this.pending >= 128 || !events[kind] || bytes.length !== 132) return false;
        let peer;
        try { peer = unpack(bytes, handle); }
        catch { return false; }
        ++this.pending;
        queueMicrotask(() => {
            --this.pending;
            if (this.closed || this.failed) return;
            try { this.handlers.onPeer(peer, events[kind]); }
            catch { this.fail("callback"); }
        });
        return true;
    }
    output(name, size, args = []) {
        if (this.closed || this.failed) return null;
        let pointer = 0;
        try {
            pointer = this.module._malloc(size);
            if (!pointer) throw new Error("allocation failed");
            const ok = this.module[name](this.id, ...args, pointer, size);
            return ok ? this.module.HEAPU8.slice(pointer, pointer + size) : null;
        } catch { this.fail("resource"); return null; }
        finally { if (pointer) this.module._free(pointer); }
    }
    key() { const value = this.output("_p2pdc_discovery_key", 20); if (!value) this.check(); return value; }
    windowKey(now) {
        if (!Number.isSafeInteger(now) || now < 0 || now > Number.MAX_SAFE_INTEGER - 300)
            throw new RangeError("invalid discovery clock");
        if (typeof this.module._p2pdc_discovery_window_key !== "function")
            throw new TypeError("missing discovery window key export");
        const value = this.output("_p2pdc_discovery_window_key", 20, [now]);
        if (!value) this.check();
        return value;
    }
    self() {
        const value = this.output("_p2pdc_discovery_self", 100);
        if (!value) { this.check(); return null; }
        return { signingKey: value.slice(0, 32), recipientKey: value.slice(32, 64),
                 instance: value.slice(64, 80), inbox: value.slice(80, 100) };
    }
    publish(now) { const value = this.output("_p2pdc_discovery_publish", 242, [now]); if (!value) this.check(); return value; }
    publishWindowed(now) {
        if (typeof this.module._p2pdc_discovery_publish_windowed !== "function")
            throw new TypeError("missing windowed publication export");
        const value = this.output("_p2pdc_discovery_publish_windowed", 242, [now]);
        if (!value) this.check();
        return value;
    }
    receive(wire, now) {
        if (this.closed || this.failed) return "closed";
        if (!(wire instanceof Uint8Array)) throw new TypeError("binary record required");
        if (byteLength.call(wire) !== 242) return this.sweep(now) ? "ignored" : "failed";
        const value = copied(wire, 242);
        let pointer = 0;
        try {
            pointer = this.module._malloc(value.length);
            if (!pointer) throw new Error("allocation failed");
            this.module.HEAPU8.set(value, pointer);
            const result = this.module._p2pdc_discovery_receive(this.id, pointer, value.length, now);
            if (result === 2) this.fail("discovery");
            return result === 0 ? "changed" : result === 1 ? "ignored" : "failed";
        } catch { this.fail("resource"); return "failed"; }
        finally { if (pointer) this.module._free(pointer); }
    }
    sweep(now) {
        if (this.closed || this.failed) return false;
        try { if (this.module._p2pdc_discovery_sweep(this.id, now)) return true; }
        catch { this.fail("resource"); return false; }
        this.fail("discovery"); return false;
    }
    current(handle, now) {
        if (!Number.isSafeInteger(handle) || handle <= 0 || handle > 0xffffffff) throw new TypeError("invalid peer handle");
        const value = this.output("_p2pdc_discovery_current", 132, [handle, now]);
        if (!value) { this.check(); return null; }
        return unpack(value, handle);
    }
    check() {
        if (this.closed || this.failed) return;
        try { if (!this.module._p2pdc_discovery_healthy(this.id)) this.fail("discovery"); }
        catch { this.fail("resource"); }
    }
    fail(reason) {
        if (this.closed || this.failed) return;
        this.failed = true;
        queueMicrotask(() => {
            if (this.closed) return;
            this.dispose();
            try { this.handlers.onError(reason); }
            catch { console.error("p2pdc: discovery error callback failed"); }
        });
    }
    dispose() { this.bridge.watchers.delete(this.id); this.module._p2pdc_discovery_destroy(this.id); }
    close() { if (!this.closed) { this.closed = true; this.dispose(); } }
}
