import { messageLimit } from "./message_limit.mjs";

const modules = new WeakMap();
const flowResults = ["accepted", "ignored", "failed", "closed"];
const sendResults = ["accepted", "notReady", "tooLarge", "backpressure", "failed", "closed"];
const failureReasons = ["signaling", "protocol", "transport", "resource", "timeout"];
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;

function copied(value, limit) {
    if (!(value instanceof Uint8Array)) throw new TypeError("binary input required");
    const size = byteLength.call(value);
    if (size > limit) throw new RangeError("binary input exceeds limit");
    const result = new Uint8Array(size);
    Uint8Array.prototype.set.call(result, value);
    return result;
}
function configBytes(options) {
    if (!options || !["offerer", "answerer"].includes(options.role)) throw new TypeError("invalid role");
    const fields = [["identitySeed", 32], ["recipientSecret", 32], ["capability", 32],
                    ["remoteSigningKey", 32], ["remoteBoxKey", 32], ["localInstance", 16],
                    ["remoteInstance", 16], ["attemptId", 16]];
    const limit = messageLimit(options.maxMessageBytes);
    const output = new Uint8Array(213);
    let position = 0;
    try {
        for (const [name, size] of fields) {
            const value = copied(options[name], size);
            try {
                if (value.length !== size) throw new TypeError(`invalid ${name}`);
                output.set(value, position); position += size;
            } finally { value.fill(0); }
        }
        output[position] = options.role === "offerer" ? 1 : 2;
        new DataView(output.buffer).setUint32(209, limit);
        return output;
    } catch (error) { output.fill(0); throw error; }
}
function validate(module, handlers, timeoutMs) {
    if (!module?.HEAPU8 || !handlers || !Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 120000)
        throw new TypeError("invalid module, handlers or deadline");
    for (const name of ["sendFrame", "bufferedAmount", "onSignal", "onDescription", "onCandidate",
                        "onReady", "onMessage", "onWritable", "onFailure", "onControl"])
        if (typeof handlers[name] !== "function") throw new TypeError(`missing ${name} callback`);
    for (const name of ["_malloc", "_free", "_p2pdc_secure_create", "_p2pdc_secure_destroy",
                        "_p2pdc_secure_description", "_p2pdc_secure_candidate", "_p2pdc_secure_receive",
                        "_p2pdc_secure_description_done", "_p2pdc_secure_candidate_done", "_p2pdc_secure_ready",
                        "_p2pdc_secure_frame", "_p2pdc_secure_send", "_p2pdc_secure_wake", "_p2pdc_secure_timeout"])
        if (typeof module[name] !== "function") throw new TypeError(`missing ${name} export`);
}
function bridgeFor(module) {
    let bridge = modules.get(module);
    if (bridge) return bridge;
    if (module.p2pdcSecureBridge) throw new TypeError("module already has a secure bridge");
    const links = new Map();
    bridge = { links,
        bytes: (id, kind, bytes) => links.get(id)?.onBytes(kind, bytes) ?? false,
        candidate: (id, op, candidate, mid) => links.get(id)?.onCandidate(op, candidate, mid) ?? false,
        number: (id, kind, value) => links.get(id)?.onNumber(kind, value) ?? false,
        frame: (id, bytes) => links.get(id)?.sendFrame(bytes) ?? 2,
        buffered: (id) => links.get(id)?.buffered() ?? 0xffffffff };
    module.p2pdcSecureBridge = bridge;
    modules.set(module, bridge);
    return bridge;
}

// Internal preauthorized bridge. The owner supplies exact installed SDP and
// verified peer bindings, closes WebRTC on failure, and owns credential buffers.
// User notifications and async browser operations are deferred and bounded.
export class WasmSecureLink {
    constructor(module, options, handlers) {
        validate(module, handlers, options?.timeoutMs);
        this.module = module; this.handlers = handlers; this.bridge = bridgeFor(module);
        this.closed = false; this.failed = false; this.pendingEvents = 0; this.pendingBytes = 0;
        this.receiveLimit = messageLimit(options.maxMessageBytes);
        this.maxPendingBytes = Math.max(1048576, this.receiveLimit);
        const config = configBytes(options);
        try { this.id = this.callRaw("_p2pdc_secure_create", config, [], true); }
        finally { config.fill(0); }
        if (!this.id) throw new Error("cannot create authenticated attempt");
        try { this.bridge.links.set(this.id, this); }
        catch (error) { module._p2pdc_secure_destroy(this.id); throw error; }
        this.timer = setTimeout(() => this.timeout(), options.timeoutMs);
    }
    callRaw(name, value, tail = [], wipe = false) {
        let pointer = 0;
        try {
            pointer = this.module._malloc(Math.max(1, value.length));
            if (!pointer) throw new Error("allocation failed");
            this.module.HEAPU8.set(value, pointer);
            return this.module[name](pointer, value.length, ...tail);
        } finally {
            if (pointer) {
                if (wipe) this.module.HEAPU8.fill(0, pointer, pointer + value.length);
                this.module._free(pointer);
            }
        }
    }
    callBytes(name, input, maximum, tail = []) {
        const value = copied(input, maximum);
        let pointer = 0;
        try {
            pointer = this.module._malloc(Math.max(1, value.length));
            if (!pointer) throw new Error("allocation failed");
            this.module.HEAPU8.set(value, pointer);
            return this.module[name](this.id, pointer, value.length, ...tail);
        } catch { this.fail("resource"); return null; }
        finally { if (pointer) this.module._free(pointer); }
    }
    sendFrame(bytes) {
        if (this.closed || this.failed) return 2;
        try {
            const value = this.handlers.sendFrame(bytes);
            return value === "accepted" ? 0 : value === "notReady" || value === "backpressure" ? 1 : 2;
        } catch { console.error("p2pdc: secure frame callback failed"); return 2; }
    }
    buffered() {
        if (this.closed || this.failed) return 0xffffffff;
        try {
            const value = this.handlers.bufferedAmount();
            if (Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff) return value;
        } catch { console.error("p2pdc: secure buffer callback failed"); }
        this.fail("callback"); return 0xffffffff;
    }
    enqueue(size, action, terminal = false) {
        if (this.closed || this.failed || this.pendingEvents >= 128 || size > this.maxPendingBytes - this.pendingBytes) return false;
        ++this.pendingEvents; this.pendingBytes += size;
        queueMicrotask(async () => {
            try { if (!this.closed && (!this.failed || terminal)) await action(); }
            catch { this.fail("callback"); }
            finally { --this.pendingEvents; this.pendingBytes -= size; }
        });
        return true;
    }
    onBytes(kind, bytes) {
        if (kind === 0) return this.enqueue(bytes.length, () => this.handlers.onSignal(bytes));
        if (kind === 1) return this.enqueue(bytes.length, () => this.applyDescription(bytes));
        if (kind === 2) return this.enqueue(bytes.length, () => this.handlers.onMessage(bytes));
        return false;
    }
    onCandidate(operation, candidate, mid) {
        return this.enqueue(candidate.length + mid.length, () => this.applyCandidate(operation, candidate, mid));
    }
    async applyDescription(bytes) {
        let success = false;
        try { await this.handlers.onDescription(bytes); success = true; }
        catch { console.error("p2pdc: remote description failed"); }
        if (!this.closed && !this.failed) this.result(this.module._p2pdc_secure_description_done(this.id, success ? 1 : 0));
    }
    async applyCandidate(operation, candidate, mid) {
        let success = false;
        try { await this.handlers.onCandidate(candidate, mid); success = true; }
        catch { console.error("p2pdc: remote candidate failed"); }
        if (!this.closed && !this.failed) this.result(this.module._p2pdc_secure_candidate_done(this.id, operation, success ? 1 : 0));
    }
    onNumber(kind, value) {
        if (kind === 2) { this.fail(failureReasons[value] ?? "resource"); return true; }
        if (kind === 0) {
            const accepted = this.enqueue(0, () => this.handlers.onReady(value));
            if (accepted) clearTimeout(this.timer);
            return accepted;
        }
        if (kind === 1) return this.enqueue(0, () => this.handlers.onWritable());
        if (kind === 3) return this.enqueue(0, () => this.handlers.onControl(value), true);
        this.fail("protocol"); return false;
    }
    fail(reason) {
        if (this.closed || this.failed) return;
        this.failed = true; clearTimeout(this.timer);
        queueMicrotask(() => {
            if (this.closed) return;
            this.dispose();
            try { this.handlers.onFailure(reason); }
            catch { console.error("p2pdc: secure failure callback failed"); }
        });
    }
    dispose() { this.bridge.links.delete(this.id); this.module._p2pdc_secure_destroy(this.id); }
    close() { if (!this.closed) { this.closed = true; clearTimeout(this.timer); this.dispose(); } }
    result(value) { return flowResults[value] ?? "failed"; }
    localDescription(bytes, now) {
        if (this.closed || this.failed) return "closed";
        return this.result(this.callBytes("_p2pdc_secure_description", bytes, 32768, [now]));
    }
    localCandidate(candidate, mid, now) {
        if (this.closed || this.failed) return "closed";
        const first = copied(candidate, 4096), second = copied(mid, 256);
        if (!first.length || !second.length) throw new TypeError("candidate and mid required");
        const packed = new Uint8Array(first.length + second.length); packed.set(first); packed.set(second, first.length);
        let pointer = 0;
        try {
            pointer = this.module._malloc(packed.length);
            if (!pointer) throw new Error("allocation failed");
            this.module.HEAPU8.set(packed, pointer);
            return this.result(this.module._p2pdc_secure_candidate(this.id, pointer, first.length,
                pointer + first.length, second.length, now));
        } catch { this.fail("resource"); return "failed"; }
        finally { if (pointer) this.module._free(pointer); }
    }
    receive(bytes, now) {
        if (this.closed || this.failed) return "closed";
        return this.result(this.callBytes("_p2pdc_secure_receive", bytes, 32768, [now]));
    }
    setTransportReady(ready, maximum) {
        if (this.closed || this.failed) return false;
        if (typeof ready !== "boolean") throw new TypeError("ready must be boolean");
        if (ready && (typeof maximum !== "number" || Number.isNaN(maximum) || maximum < 0 ||
                      (maximum !== 0 && maximum < 1024))) { this.fail("transportLimit"); return false; }
        return this.module._p2pdc_secure_ready(this.id, ready ? 1 : 0) === 0 && !this.failed;
    }
    onFrame(bytes) {
        if (this.closed || this.failed) return false;
        return this.callBytes("_p2pdc_secure_frame", bytes, 1024) === 0 && !this.failed;
    }
    send(bytes) {
        if (this.closed || this.failed) return "closed";
        if (!(bytes instanceof Uint8Array)) throw new TypeError("binary input required");
        if (byteLength.call(bytes) > this.receiveLimit) return "tooLarge";
        return sendResults[this.callBytes("_p2pdc_secure_send", bytes, this.receiveLimit)] ?? "failed";
    }
    wake() { if (!this.closed && !this.failed) this.module._p2pdc_secure_wake(this.id); }
    timeout() { if (!this.closed && !this.failed) this.module._p2pdc_secure_timeout(this.id); }
}
