import { WasmSecureLink } from "./wasm_secure_link.mjs";
import { BrowserWebRtcTransport } from "./webrtc_transport.mjs";
import { DiscoveryClock } from "./discovery_clock.mjs";
import { messageLimit } from "./message_limit.mjs";

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;
const copyRoute = (route) => ({ ...route, selectedPair: route.selectedPair ? { ...route.selectedPair } : null });

// One preauthorized attempt. The owner supplies ordered signaling delivery and
// verified peer bindings; this object does not authenticate advertisements.
export class BrowserConnection {
    constructor(module, options, handlers) {
        for (const name of ["onSignal", "onState", "onRoute", "onMessage", "onWritable", "onError"]) {
            if (typeof handlers?.[name] !== "function") throw new TypeError(`missing ${name} callback`);
        }
        if (!options?.secure || !options?.transport || options.secure.role !== options.transport.role)
            throw new TypeError("inconsistent connection roles");
        for (const name of ["maxMessageBytes", "maxBufferBytes", "maxInboundBytes"]) {
            if (options.transport[name] !== undefined && options.transport[name] < 1024)
                throw new RangeError(`${name} is too small for secure frames`);
        }
        this.handlers = handlers; this.role = options.secure.role;
        this.receiveLimit = messageLimit(options.secure.maxMessageBytes);
        this.clock = new DiscoveryClock(options.now ?? (() => Math.floor(Date.now() / 1000)));
        this.state = "connecting"; this.started = false; this.authenticated = false; this.messageLimit = null; this.epoch = 0;
        this.transport = new BrowserWebRtcTransport(options.transport, this.transportHandlers());
        try { this.link = new WasmSecureLink(module, options.secure, this.secureHandlers()); }
        catch (error) { this.transport.close(); throw error; }
    }
    transportHandlers() {
        return {
            onDescription: (value) => this.localDescription(value),
            onCandidate: (value) => this.localCandidate(value),
            onState: (value) => this.transportState(value),
            onBinary: (value) => { if (!this.link.onFrame(value)) this.fail("protocol"); },
            onRoute: (value) => this.notify("onRoute", copyRoute(value)),
            onError: (reason) => this.fail(reason),
            onWritable: () => this.link.wake(),
        };
    }
    secureHandlers() {
        return {
            sendFrame: (frame) => this.transport.send(frame), bufferedAmount: () => this.transport.bufferedAmount(),
            onSignal: (wire) => this.handlers.onSignal(wire),
            onDescription: (sdp) => this.transport.setRemoteDescription({
                type: this.role === "offerer" ? "answer" : "offer", sdp: decoder.decode(sdp) }),
            onCandidate: (candidate, mid) => this.transport.addRemoteCandidate({
                candidate: decoder.decode(candidate), sdpMid: decoder.decode(mid) }),
            onControl: () => {}, onWritable: () => this.notify("onWritable"),
            onFailure: (reason) => this.fail(reason), onReady: (limit) => this.secureReady(limit),
            onMessage: (message) => this.notify("onMessage", message),
        };
    }
    localDescription(value) {
        try {
            if (this.link.localDescription(encoder.encode(value.sdp), this.clock.read()) !== "accepted")
                this.fail("signaling");
        } catch { this.fail("clock"); }
    }
    localCandidate(value) {
        if (typeof value.sdpMid !== "string" || !value.sdpMid) { this.fail("candidate"); return; }
        try {
            if (this.link.localCandidate(encoder.encode(value.candidate), encoder.encode(value.sdpMid),
                this.clock.read()) !== "accepted") this.fail("signaling");
        } catch { this.fail("clock"); }
    }
    transportState(value) {
        if (value === "failed") { this.fail("transport"); return; }
        if (!this.link.setTransportReady(value === "ready", this.transport.maxMessageSize())) {
            this.fail("transport"); return;
        }
        if (this.authenticated) this.setState(value === "ready" ? "ready" : "suspended");
    }
    secureReady(limit) {
        if (this.state === "failed" || this.state === "closed") return;
        if (!Number.isSafeInteger(limit) || limit <= 0 || limit > this.receiveLimit) { this.fail("protocol"); return; }
        this.messageLimit = limit;
        this.authenticated = true;
        this.setState(this.transport.status === "ready" ? "ready" : "suspended");
    }
    notify(name, value) {
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (epoch !== this.epoch || this.state === "closed" || this.state === "failed") return;
            try { this.handlers[name](value); }
            catch { this.fail("callback"); }
        });
    }
    setState(value) {
        if (this.state === value) return;
        this.state = value; this.notify("onState", value);
    }
    async start() {
        if (this.started || this.state !== "connecting") throw new Error("connection cannot be started");
        this.started = true;
        if (this.role === "answerer") return;
        try { await this.transport.startOffer(); }
        catch { this.fail("transport"); }
    }
    receive(wire) {
        if (this.state === "failed") return "failed";
        if (!this.started || this.state === "closed") return "closed";
        try {
            const result = this.link.receive(wire, this.clock.read());
            if (result === "failed") this.fail("signaling");
            return result;
        } catch { this.fail("signaling"); return "failed"; }
    }
    send(bytes) {
        if (this.state === "closed") return "closed";
        if (this.state === "failed") return "failed";
        if (this.state !== "ready") return "notReady";
        if (bytes instanceof Uint8Array && byteLength.call(bytes) > this.messageLimit) return "tooLarge";
        return this.link.send(bytes);
    }
    route() { return copyRoute(this.transport.route); }
    maxMessageSize() { return this.authenticated && !["closed", "failed"].includes(this.state) ? this.messageLimit : null; }
    pollRoute() { return this.transport.pollRoute().then(copyRoute); }
    fail(reason) {
        if (this.state === "failed" || this.state === "closed") return;
        this.state = "failed"; ++this.epoch; this.transport.close(); this.link.close();
        const epoch = this.epoch;
        queueMicrotask(() => {
            if (epoch !== this.epoch || this.state !== "failed") return;
            try { this.handlers.onState("failed"); }
            catch (error) { console.error("p2pdc: connection state callback failed", error); }
            if (epoch !== this.epoch || this.state !== "failed") return;
            try { this.handlers.onError(reason); }
            catch { console.error("p2pdc: connection error callback failed"); }
        });
    }
    close() {
        if (this.state === "closed") return;
        this.state = "closed"; ++this.epoch; this.transport.close(); this.link.close();
    }
}
