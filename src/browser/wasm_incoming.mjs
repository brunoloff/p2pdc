import { DiscoveryClock } from "./discovery_clock.mjs";

const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;

// Internal admission only. A matched offer is not application authorization;
// the caller must retain the discovery handles and recheck them before consent.
export class WasmIncoming {
    constructor(lease, watch, options = {}) {
        const module = lease?.discovery?.module;
        if (!module || watch?.discovery?.module !== module || typeof module._p2pdc_discovery_incoming !== "function" ||
            typeof lease.self !== "function" || typeof watch.current !== "function" || typeof watch.healthy !== "function")
            throw new TypeError("invalid incoming admission dependencies");
        this.lease = lease; this.watch = watch; this.module = module;
        this.localDiscovery = lease.discovery; this.watchDiscovery = watch.discovery;
        this.localId = lease.discovery.id; this.watchId = watch.discovery.id;
        this.clock = new DiscoveryClock(options.now ?? (() => Math.floor(Date.now() / 1000)));
        this.closed = false; this.failed = false;
    }
    receive(wire, inspectOnly = false) {
        if (this.closed) return { status: "closed" };
        if (this.failed) return { status: "failed" };
        if (!(wire instanceof Uint8Array)) throw new TypeError("binary signaling record required");
        if (typeof inspectOnly !== "boolean") throw new TypeError("invalid inspection mode");
        if (this.lease.discovery !== this.localDiscovery || this.watch.discovery !== this.watchDiscovery ||
            this.localDiscovery.id !== this.localId || this.watchDiscovery.id !== this.watchId ||
            this.lease.closed || this.lease.failed || this.watch.closed || this.watch.failed ||
            this.localDiscovery.closed || this.localDiscovery.failed || this.watchDiscovery.closed ||
            this.watchDiscovery.failed || this.watch.mailbox?.failed || this.watch.mailbox?.closed ||
            this.lease.outbox?.closed) {
            this.failed = true; return { status: "failed" };
        }
        const size = byteLength.call(wire);
        if (size > 32768) return { status: "ignored" };
        if (!this.lease.self() || !this.watch.healthy()) return { status: "notReady" };
        let pointer = 0;
        try {
            const now = this.clock.read();
            const copy = new Uint8Array(size); Uint8Array.prototype.set.call(copy, wire);
            pointer = this.module._malloc(size + 20);
            if (!pointer) throw new Error("allocation failed");
            this.module.HEAPU8.set(copy, pointer);
            const result = this.module._p2pdc_discovery_incoming(this.localId, this.watchId,
                pointer, size, now, inspectOnly ? 1 : 0, pointer + size, 20);
            if (result === 0) return { status: "ignored" };
            if (result === 3) return { status: "unknownPeer" };
            if (result === 4) return { status: "nonOffer" };
            if (result === 5 && inspectOnly) return { status: "otherOffer" };
            if (result !== 1) throw new Error("incoming admission failed");
            const output = this.module.HEAPU8.slice(pointer + size, pointer + size + 20);
            const handle = new DataView(output.buffer, output.byteOffset, 4).getUint32(0);
            const peer = this.watch.current(handle);
            if (!peer || !this.lease.self()) return { status: "ignored" };
            return { status: "admitted", peer, attemptId: output.slice(4), wire: copy };
        } catch { this.failed = true; return { status: "failed" }; }
        finally { if (pointer) this.module._free(pointer); }
    }
    close() { this.closed = true; }
}
