import { ProxyError } from "./proxy_codec.mjs";
import { DataLink } from "./peerjs_link.mjs";
import { PeerJsSocket } from "./peerjs_socket.mjs";
import { randomId } from "./store_limits.mjs";

// The PeerJS client's own ICE defaults (peers/peerjs lib/util.ts): Google's STUN server and
// the public TURN relays PeerJS runs for its users.
export const peerJsIceServers = [
    { urls: "stun:stun.l.google.com:19302" },
    { urls: ["turn:eu-0.turn.peerjs.com:3478", "turn:us-0.turn.peerjs.com:3478"], username: "peerjs", credential: "peerjsp" },
];
const maxSdpChars = 65536, maxCandidateChars = 4096, maxNegotiations = 32, maxQueuedCandidates = 64;

// The other side is not connected to the server, or its data connection closed.
export class PeerGone extends ProxyError { constructor(what) { super("network_error", what); } }

const rtcError = (error) => error instanceof ProxyError ? error : new ProxyError("network_error", `WebRTC: ${error?.message ?? error}`);
function sessionDescription(value, type) {
    if (!value || value.type !== type || typeof value.sdp !== "string" || value.sdp.length > maxSdpChars)
        throw new ProxyError("network_error", `invalid ${type} from the peer`);
    return { type, sdp: value.sdp };
}
function iceCandidate(value) {
    if (!value || typeof value.candidate !== "string" || value.candidate.length > maxCandidateChars)
        throw new ProxyError("network_error", "invalid ICE candidate from the peer");
    // Emscripten's JS optimizer, which bundles this file, rejects destructuring defaults.
    return { candidate: value.candidate, sdpMid: value.sdpMid ?? null, sdpMLineIndex: value.sdpMLineIndex ?? null,
             usernameFragment: value.usernameFragment ?? null };
}

// One data connection being set up with peer, as the PeerJS client does: the offerer creates
// the channel, OFFER and ANSWER carry the descriptions, CANDIDATE each ICE candidate.
// Candidates that arrive before the remote description wait for it.
class Negotiation {
    constructor(endpoint, peer, connectionId) {
        this.endpoint = endpoint; this.peer = peer; this.connectionId = connectionId;
        this.pc = new endpoint.rtc.RTCPeerConnection({ iceServers: endpoint.rtc.iceServers });
        this.queued = []; this.described = false; this.settled = false;
        this.done = new Promise((resolve, reject) => { this.resolve = resolve; this.reject = reject; });
        this.timer = setTimeout(() => this.fail(new ProxyError("timeout", "WebRTC data connection did not open")), endpoint.timeoutMs);
        this.pc.onicecandidate = (event) => {
            if (event.candidate?.candidate) this.signal("CANDIDATE", { candidate: event.candidate, type: "data", connectionId });
        };
        this.pc.onconnectionstatechange = () => {
            if (this.pc.connectionState === "failed") this.fail(new ProxyError("network_error", "WebRTC found no path to the peer"));
        };
        endpoint.negotiations.set(connectionId, this);
    }
    signal(type, payload) {
        try { this.endpoint.socket.send(type, this.peer, payload); } catch (error) { this.fail(rtcError(error)); }
    }
    channel(channel) {
        channel.onopen = () => this.open(channel);
        if (channel.readyState === "open") this.open(channel);
    }
    open(channel) {
        if (this.settled) return;
        this.finish();
        const link = new DataLink(this.peer, this.pc, channel);
        link.released = () => this.endpoint.links.delete(link);
        this.endpoint.links.add(link);
        this.resolve(link);
    }
    async describe(value, type) {
        await this.pc.setRemoteDescription(sessionDescription(value, type));
        this.described = true;
        for (const candidate of this.queued.splice(0)) await this.pc.addIceCandidate(candidate);
    }
    async candidate(value) {
        const candidate = iceCandidate(value);
        if (this.described) await this.pc.addIceCandidate(candidate);
        else if (this.queued.length < maxQueuedCandidates) this.queued.push(candidate);
    }
    fail(error) {
        if (this.settled) return;
        this.finish();
        try { this.pc.close(); } catch { /* already closed */ }
        this.reject(error);
    }
    finish() {
        this.settled = true;
        clearTimeout(this.timer);
        this.endpoint.negotiations.delete(this.connectionId);
    }
}

// An ID on a PeerJS server that opens data connections to other IDs (connect) and, when given
// an incoming handler, accepts them. The server only relays the signalling: once a connection
// is open it no longer depends on the server.
export class PeerJsEndpoint {
    constructor(url, key, id, handlers, options) {
        this.handlers = handlers; this.rtc = options.rtc; this.timeoutMs = options.timeoutMs;
        this.maxLinks = options.maxLinks;
        this.negotiations = new Map(); // connectionId -> Negotiation
        this.links = new Set();
        this.socket = new PeerJsSocket(url, key, id, { signal: (message) => this.signal(message), gone: (peer) => this.gone(peer),
                                                       close: (error) => this.lost(error) }, options.timeoutMs);
    }
    register() { return this.socket.register(); }
    // A DataLink to peer, or a PeerGone when the server says peer is not there.
    async connect(peer) {
        if (await this.register() !== "open") throw new ProxyError("rejected", "PeerJS ID is taken");
        if (this.negotiations.size >= maxNegotiations) throw new ProxyError("network_error", "too many data connections opening");
        const connectionId = `dc_${randomId()}`, negotiation = new Negotiation(this, peer, connectionId);
        negotiation.channel(negotiation.pc.createDataChannel(connectionId, { ordered: true }));
        try {
            await negotiation.pc.setLocalDescription(await negotiation.pc.createOffer());
            const sdp = sessionDescription(negotiation.pc.localDescription, "offer");
            negotiation.signal("OFFER", { sdp, type: "data", connectionId, label: connectionId, reliable: true, serialization: "raw" });
        } catch (error) { negotiation.fail(rtcError(error)); }
        return negotiation.done;
    }
    signal(message) {
        const payload = message.payload;
        if (!payload || payload.type !== "data" || typeof payload.connectionId !== "string" ||
            !/^[\w-]{1,64}$/.test(payload.connectionId)) return;
        const negotiation = this.negotiations.get(payload.connectionId);
        if (message.type === "OFFER") { if (!negotiation) this.answer(message.src, payload); return; }
        if (negotiation?.peer !== message.src) return;
        const step = message.type === "ANSWER" ? negotiation.describe(payload.sdp, "answer") : negotiation.candidate(payload.candidate);
        step.catch((error) => negotiation.fail(rtcError(error)));
    }
    async answer(peer, payload) {
        if (!this.handlers.incoming || this.negotiations.size >= maxNegotiations || this.links.size >= this.maxLinks) return;
        const negotiation = new Negotiation(this, peer, payload.connectionId);
        negotiation.pc.ondatachannel = (event) => negotiation.channel(event.channel);
        // Whoever offered learns of a failure from its own side; the host just drops it.
        negotiation.done.then((link) => this.handlers.incoming(link), () => {});
        try {
            await negotiation.describe(payload.sdp, "offer");
            await negotiation.pc.setLocalDescription(await negotiation.pc.createAnswer());
            const sdp = sessionDescription(negotiation.pc.localDescription, "answer");
            negotiation.signal("ANSWER", { sdp, type: "data", connectionId: payload.connectionId });
        } catch (error) { negotiation.fail(rtcError(error)); }
    }
    // EXPIRE or LEAVE: peer is not on the server, so connections still being set up fail.
    gone(peer) {
        for (const negotiation of [...this.negotiations.values()])
            if (negotiation.peer === peer) negotiation.fail(new PeerGone("the peer is not connected to the PeerJS server"));
    }
    // Open data connections outlive the server connection; those being set up do not.
    lost(error) {
        for (const negotiation of [...this.negotiations.values()]) negotiation.fail(error);
        this.handlers.close?.(error);
    }
    close() {
        this.socket.close();
        for (const negotiation of [...this.negotiations.values()]) negotiation.fail(new ProxyError("network_error", "PeerJS endpoint closed"));
        for (const link of [...this.links]) link.close();
    }
}
