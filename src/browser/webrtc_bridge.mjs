import { BrowserWebRtcTransport } from "./webrtc_transport.mjs";

const encoder = new TextEncoder();
const states = new Map([["connecting", 0], ["ready", 1], ["suspended", 2], ["failed", 3]]);
const sends = new Map([["accepted", 0], ["notReady", 1], ["tooLarge", 2],
                       ["backpressure", 3], ["failed", 4], ["closed", 5]]);
const candidates = new Map([["unknown", 0], ["host", 1], ["serverReflexive", 2],
                            ["peerReflexive", 3], ["relayed", 4]]);
const routes = new Map([["unknown", 0], ["direct", 1], ["turnRelayed", 2]]);

// Owns only the browser transport. C++ owns authentication, signaling and callbacks.
class WebRtcBridge {
    constructor(deliver, makeTransport) {
        if (typeof deliver !== "function" || typeof makeTransport !== "function") throw new TypeError("invalid bridge");
        this.deliver = deliver; this.makeTransport = makeTransport; this.entries = new Map(); this.next = 1;
    }
    begin(role, relayPolicy, limits) {
        if (this.entries.size >= 256 || this.next > 0xffffffff) throw new RangeError("transport limit reached");
        const id = this.next++;
        this.entries.set(id, { id, options: { role, relayPolicy, ...limits, iceServers: [] },
            transport: null, active: true, started: false, description: false, candidates: new Set() });
        return id;
    }
    entry(id) {
        const entry = this.entries.get(id);
        if (!entry?.active) throw new TypeError("unknown transport");
        return entry;
    }
    addServer(id, url, username, credential) {
        const entry = this.entry(id);
        if (entry.transport || entry.options.iceServers.length >= 16) throw new TypeError("invalid ICE server");
        entry.options.iceServers.push({ urls: url, username, credential });
    }
    emit(entry, kind, value = 0, first = null, second = null) {
        if (!entry.active) return false;
        try { if (this.deliver(entry.id, kind, value, first, second)) return true; }
        catch { console.error("p2pdc: WebRTC bridge callback failed"); }
        this.close(entry.id); return false;
    }
    handlers(entry) {
        return {
            onDescription: (value) => this.emit(entry, 0, 0, encoder.encode(value.sdp)),
            onCandidate: (value) => {
                if (typeof value.sdpMid !== "string" || !value.sdpMid) return this.emit(entry, 5);
                return this.emit(entry, 1, 0, encoder.encode(value.candidate), encoder.encode(value.sdpMid));
            },
            onState: (state) => this.emit(entry, 2, states.get(state) ?? 3),
            onBinary: (value) => this.emit(entry, 3, 0, value),
            onRoute: (route) => this.route(entry, route),
            onError: (_reason, category) => this.emit(entry, 5, category === "resource" ? 1 : 0),
            onWritable: () => this.emit(entry, 6),
            onDatagram: (value) => this.emit(entry, 10, 0, value),
        };
    }
    route(entry, route) {
        const local = candidates.get(route.selectedPair?.local) ?? 0;
        const remote = candidates.get(route.selectedPair?.remote) ?? 0;
        const value = (routes.get(route.current) ?? 0) | ((route.turnHistory === "used" ? 1 : 0) << 2) |
            (route.selectedPair ? 1 << 3 : 0) | (local << 4) | (remote << 8);
        return this.emit(entry, 4, value);
    }
    open(id) {
        const entry = this.entry(id);
        if (entry.transport) throw new TypeError("transport already open");
        entry.transport = this.makeTransport(entry.options, this.handlers(entry));
    }
    start(id) {
        const entry = this.entry(id);
        if (!entry.transport || entry.started) throw new TypeError("transport cannot start");
        entry.started = true;
        void entry.transport.startOffer().catch(() => this.emit(entry, 5));
    }
    description(id, sdp) {
        const entry = this.entry(id);
        if (!entry.transport || entry.description) throw new TypeError("description already pending");
        entry.description = true;
        const type = entry.options.role === "offerer" ? "answer" : "offer";
        void entry.transport.setRemoteDescription({ type, sdp }).then(
            () => { entry.description = false; this.emit(entry, 7, 1); },
            () => { entry.description = false; this.emit(entry, 7, 0); });
    }
    candidate(id, operation, candidate, mid) {
        const entry = this.entry(id);
        if (!entry.transport || !Number.isSafeInteger(operation) || operation < 1 || operation > 0xffffffff ||
            entry.candidates.size >= 64 || entry.candidates.has(operation)) throw new TypeError("invalid candidate operation");
        entry.candidates.add(operation);
        void entry.transport.addRemoteCandidate({ candidate, sdpMid: mid }).then(
            () => { entry.candidates.delete(operation); this.emit(entry, 8, operation); },
            () => { entry.candidates.delete(operation); this.emit(entry, 9, operation); });
    }
    send(id, bytes) { return sends.get(this.entry(id).transport.send(bytes)) ?? 4; }
    enableDatagrams(id) { this.entry(id).transport.enableDatagrams(); }
    datagramsAvailable(id) { return this.entry(id).transport.datagramsAvailable(); }
    sendDatagram(id, bytes) { return sends.get(this.entry(id).transport.sendDatagram(bytes)) ?? 4; }
    buffered(id) { return this.entry(id).transport.bufferedAmount(); }
    maximum(id) { return this.entry(id).transport.maxMessageSize(); }
    poll(id) {
        const entry = this.entry(id);
        void entry.transport.pollRoute().catch(() => this.emit(entry, 5));
    }
    close(id) {
        const entry = this.entries.get(id);
        if (!entry) return;
        entry.active = false; this.entries.delete(id); entry.transport?.close(); entry.candidates.clear();
    }
}
export function createWebRtcBridge(deliver, makeTransport = (options, handlers) => new BrowserWebRtcTransport(options, handlers)) {
    return new WebRtcBridge(deliver, makeTransport);
}
