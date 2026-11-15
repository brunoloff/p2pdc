import { routeKind, selectedPair } from "./webrtc_route.mjs";
import { settings } from "./webrtc_settings.mjs";
import { observeWithDeadline, recoverExpiredRequest } from "./webrtc_stats.mjs";

const label = "p2pdc/reliable/v1";
const maxSignalBytes = 32768;
const routeRetryMs = 250;
const emptyRoute = (used) => ({ current: "unknown", turnHistory: used ? "used" : "unknown", selectedPair: null });

// Raw transport only: signaling and peer authentication must be supplied by
// the caller. All methods and callbacks run on one browser event loop.
export class BrowserWebRtcTransport {
    constructor(options, handlers) {
        const limits = settings(options, handlers);
        this.role = options.role;
        this.relayPolicy = options.relayPolicy;
        this.handlers = handlers;
        this.limits = limits;
        const create = options.peerConnectionFactory ?? ((config) => new RTCPeerConnection(config));
        this.peer = create({ iceServers: limits.servers,
                             iceTransportPolicy: options.relayPolicy === "requireLocalRelay" ? "relay" : "all" });
        this.channel = null;
        this.status = "connecting";
        this.closed = false;
        this.started = false;
        this.remoteSet = false;
        this.remoteSetting = false;
        this.pendingCandidates = [];
        this.localCandidates = [];
        this.localCandidateBytes = 0;
        this.localCandidateCount = 0;
        this.localDescriptionQueued = false;
        this.candidateBytes = 0;
        this.candidateCount = 0;
        this.candidateDrain = null;
        this.pendingBytes = 0;
        this.inbound = [];
        this.inboundScheduled = false;
        this.epoch = 0;
        this.deliveryEpoch = 0;
        this.stateEpoch = 0;
        this.readyNotified = false;
        this.statsFlight = null;
        this.statsTimer = null;
        this.cancelStatsDeadline = null;
        this.turnUsed = false;
        this.route = emptyRoute(false);
        this.bindPeer();
    }
    emit(name, ...args) {
        const epoch = this.deliveryEpoch;
        queueMicrotask(() => {
            if (this.closed || epoch !== this.deliveryEpoch) return;
            try { this.handlers[name](...args); }
            catch {
                if (name === "onError") console.error("p2pdc: WebRTC error callback failed");
                else this.fail("callback failed");
            }
        });
    }
    emitState(value) {
        const epoch = ++this.stateEpoch;
        this.readyNotified = false;
        queueMicrotask(() => {
            if (this.closed || epoch !== this.stateEpoch || this.status !== value) return;
            if (value === "ready") this.readyNotified = true;
            try { this.handlers.onState(value); }
            catch { this.fail("callback failed"); }
            if (value === "ready") this.scheduleInbound();
        });
    }
    bindPeer() {
        this.peer.onicecandidate = (event) => {
            if (!this.closed && event.candidate?.candidate) this.localCandidate(event.candidate);
        };
        this.peer.ondatachannel = (event) => this.attach(event.channel);
        this.peer.onconnectionstatechange = () => this.update();
        this.peer.oniceconnectionstatechange = () => this.update();
    }
    localCandidate(candidate) {
        try {
            const value = candidate.toJSON();
            if (!value || typeof value.candidate !== "string" || !value.candidate || value.candidate.length > 4096 ||
                (value.sdpMid != null && (typeof value.sdpMid !== "string" || value.sdpMid.length > 256)) ||
                (value.sdpMLineIndex != null && (!Number.isInteger(value.sdpMLineIndex) ||
                 value.sdpMLineIndex < 0 || value.sdpMLineIndex > 65535)) ||
                (value.sdpMid == null && value.sdpMLineIndex == null) ||
                (value.usernameFragment != null && (typeof value.usernameFragment !== "string" ||
                 value.usernameFragment.length > 256))) throw new TypeError("invalid local candidate");
            const encoder = new TextEncoder();
            const candidateSize = encoder.encode(value.candidate).length;
            const size = candidateSize + encoder.encode(value.sdpMid ?? "").length +
                         encoder.encode(value.usernameFragment ?? "").length;
            if (candidateSize > 4096 || size > 4608 ||
                this.localCandidateCount >= 63 || size > 65536 - this.localCandidateBytes)
                throw new RangeError("local candidate limits exceeded");
            const copy = { candidate: value.candidate, sdpMid: value.sdpMid ?? null,
                           sdpMLineIndex: value.sdpMLineIndex ?? null,
                           usernameFragment: value.usernameFragment ?? null };
            ++this.localCandidateCount; this.localCandidateBytes += size;
            if (this.localDescriptionQueued) this.emit("onCandidate", copy);
            else this.localCandidates.push(copy);
        } catch { this.fail("invalid local candidate"); }
    }
    emitLocalDescription() {
        const description = this.peer.localDescription.toJSON();
        this.emit("onDescription", description);
        this.localDescriptionQueued = true;
        for (const candidate of this.localCandidates) this.emit("onCandidate", candidate);
        this.localCandidates.length = 0;
    }
    attach(channel) {
        if (this.closed) { channel.close(); return; }
        if (this.channel || channel.label !== label || !channel.ordered ||
            channel.maxPacketLifeTime !== null || channel.maxRetransmits !== null) {
            channel.close(); this.fail("unexpected or unreliable data channel"); return;
        }
        this.channel = channel;
        channel.binaryType = "arraybuffer";
        channel.bufferedAmountLowThreshold = 0;
        channel.onopen = () => this.update();
        channel.onclose = () => this.fail("data channel closed");
        channel.onerror = () => this.fail("data channel error");
        channel.onbufferedamountlow = () => this.emit("onWritable");
        channel.onmessage = (event) => this.receive(event.data);
        this.update();
    }
    receive(value) {
        if (this.closed || this.status === "failed") return;
        if (!(value instanceof ArrayBuffer)) { this.fail("non-binary data channel message"); return; }
        const size = value.byteLength;
        if (size > this.limits.maxMessageBytes || this.inbound.length >= this.limits.maxInboundMessages ||
            size > this.limits.maxInboundBytes - this.pendingBytes) { this.fail("receive limit exceeded", "resource"); return; }
        try { this.inbound.push(new Uint8Array(value).slice()); }
        catch { this.fail("receive allocation failed", "resource"); return; }
        this.pendingBytes += size;
        this.scheduleInbound();
    }
    scheduleInbound() {
        if (this.inboundScheduled || !this.inbound.length || this.closed || this.status === "failed") return;
        this.inboundScheduled = true;
        queueMicrotask(() => { this.inboundScheduled = false; this.drainInbound(); });
    }
    drainInbound() {
        if (this.closed || this.status === "failed" || !this.inbound.length) return;
        if (["connecting", "suspended"].includes(this.status) && this.channel?.readyState === "open") return;
        if (this.status !== "ready") { this.fail("binary received before readiness"); return; }
        if (!this.readyNotified) { this.scheduleInbound(); return; }
        for (let count = 0; count < 16 && this.inbound.length && !this.closed && this.status === "ready"; ++count) {
            const bytes = this.inbound.shift();
            this.pendingBytes -= bytes.length;
            try { this.handlers.onBinary(bytes); }
            catch { this.fail("callback failed"); }
        }
        this.scheduleInbound();
    }
    connected() {
        return this.peer.connectionState === "connected" &&
               ["connected", "completed"].includes(this.peer.iceConnectionState) &&
               this.channel?.readyState === "open";
    }
    update() {
        if (this.closed || this.status === "failed") return;
        ++this.epoch;
        if (["failed", "closed"].includes(this.peer.connectionState) || this.peer.iceConnectionState === "failed") {
            this.fail("peer connection failed"); return;
        }
        if (!this.connected()) {
            this.cancelRouteRetry();
            this.invalidateRoute();
            if (this.status === "ready") { this.status = "suspended"; this.emitState(this.status); }
            return;
        }
        if (this.relayPolicy === "automatic" && this.status !== "ready") {
            this.status = "ready"; this.emitState(this.status);
        }
        void this.pollRoute();
    }
    invalidateRoute() {
        this.route = emptyRoute(this.turnUsed);
        this.emit("onRoute", this.route);
    }
    cancelRouteRetry() {
        if (this.statsTimer !== null) clearTimeout(this.statsTimer);
        this.statsTimer = null;
    }
    retryRoute() {
        if (this.relayPolicy !== "requireLocalRelay" || this.statsTimer !== null ||
            this.closed || this.status === "failed" || !this.connected()) return;
        this.statsTimer = setTimeout(() => {
            this.statsTimer = null;
            if (!this.closed && this.status !== "failed" && this.connected()) void this.pollRoute();
        }, routeRetryMs);
    }
    expireRoute() {
        this.cancelStatsDeadline = null;
        if (this.closed || this.status === "failed" || !this.connected()) return;
        ++this.epoch;
        this.invalidateRoute();
        if (this.relayPolicy === "requireLocalRelay" && this.status === "ready") {
            this.status = "suspended"; this.emitState(this.status);
        }
    }
    pollRoute() {
        if (!this.connected() || this.closed || this.status === "failed") return Promise.resolve(this.route);
        if (this.statsFlight) return this.statsFlight;
        const epoch = this.epoch;
        let request;
        try { request = Promise.resolve(this.peer.getStats()); }
        catch (error) { request = Promise.reject(error); }
        const timed = observeWithDeadline(request, () => this.expireRoute());
        this.cancelStatsDeadline = timed.cancel;
        const task = this.observeRoute(epoch, timed.result);
        this.statsFlight = task;
        task.then(() => {
            if (this.statsFlight !== task) return;
            if (this.cancelStatsDeadline) { this.cancelStatsDeadline(); this.cancelStatsDeadline = null; }
            if (timed.expired()) return;
            this.statsFlight = null;
            if (this.epoch !== epoch && !this.closed && this.status !== "failed" && this.connected())
                void this.pollRoute();
        }, () => { this.statsFlight = null; this.fail("route observation failed"); });
        recoverExpiredRequest(request, task, timed.expired, () => {
            if (this.statsFlight !== task) return;
            this.statsFlight = null;
            if (!this.closed && this.status !== "failed" && this.connected()) void this.pollRoute();
        });
        return task;
    }
    async observeRoute(epoch, request) {
        let report;
        try { report = await request; }
        catch {
            if (!this.closed && epoch === this.epoch && this.connected()) {
                this.invalidateRoute();
                if (this.relayPolicy === "requireLocalRelay" && this.status === "ready") {
                    this.status = "suspended"; this.emitState(this.status);
                }
                this.retryRoute();
            }
            return this.route;
        }
        if (this.closed || epoch !== this.epoch || !this.connected()) return this.route;
        let pair;
        try { pair = selectedPair(report); }
        catch { pair = null; }
        if (pair) this.cancelRouteRetry(); else this.retryRoute();
        const kind = routeKind(pair);
        if (kind === "turnRelayed") this.turnUsed = true;
        this.route = { current: kind, turnHistory: this.turnUsed ? "used" : "unknown", selectedPair: pair };
        this.emit("onRoute", this.route);
        if (this.relayPolicy === "requireLocalRelay" && !pair && this.status === "ready") {
            this.status = "suspended"; this.emitState(this.status);
        }
        if (this.relayPolicy === "requireLocalRelay" && pair && pair.local !== "relayed") {
            this.fail("local relay required"); return this.route;
        }
        if (this.status !== "ready" && (this.relayPolicy === "automatic" || pair?.local === "relayed")) {
            this.status = "ready"; this.emitState(this.status);
        }
        return this.route;
    }
    fail(reason, category = "transport") {
        if (this.closed || this.status === "failed") return;
        this.status = "failed";
        ++this.epoch;
        ++this.deliveryEpoch;
        this.route = emptyRoute(this.turnUsed);
        this.release();
        this.emit("onRoute", this.route);
        this.emit("onError", reason, category);
        this.emitState(this.status);
    }
    release() {
        this.cancelRouteRetry();
        if (this.cancelStatsDeadline) { this.cancelStatsDeadline(); this.cancelStatsDeadline = null; }
        if (this.channel) {
            this.channel.onopen = this.channel.onclose = this.channel.onerror = null;
            this.channel.onmessage = this.channel.onbufferedamountlow = null;
            this.channel.close();
        }
        this.peer.onicecandidate = this.peer.ondatachannel = null;
        this.peer.onconnectionstatechange = this.peer.oniceconnectionstatechange = null;
        this.peer.close();
        this.pendingCandidates.length = 0;
        this.localCandidates.length = 0;
        this.candidateBytes = 0;
        this.candidateCount = 0;
        this.inbound.length = 0;
        this.pendingBytes = 0;
    }
    close() {
        if (this.closed) return;
        this.closed = true;
        this.status = "closed";
        ++this.epoch;
        ++this.deliveryEpoch;
        this.route = emptyRoute(this.turnUsed);
        this.release();
    }
    bufferedAmount() { return this.channel ? this.channel.bufferedAmount : null; }
    maxMessageSize() {
        const value = this.peer.sctp?.maxMessageSize;
        return typeof value === "number" && !Number.isNaN(value) && value >= 0 ? value : null;
    }
    send(bytes) {
        if (this.closed) return "closed";
        if (this.status === "failed") return "failed";
        if (this.status !== "ready" || !this.connected()) return "notReady";
        if (!(bytes instanceof Uint8Array)) throw new TypeError("binary message required");
        const maximum = this.maxMessageSize();
        if (maximum === null) return "notReady";
        const transportLimit = maximum === 0 || maximum === Infinity ? this.limits.maxMessageBytes : maximum;
        if (bytes.length > Math.min(transportLimit, this.limits.maxMessageBytes)) return "tooLarge";
        if (bytes.length > this.limits.maxBufferBytes - this.channel.bufferedAmount) return "backpressure";
        try { this.channel.send(bytes.slice()); return "accepted"; }
        catch (error) {
            if (error?.name === "OperationError") return "backpressure";
            this.fail("data channel send failed"); return "failed";
        }
    }
    async startOffer() {
        if (this.closed || this.started || this.role !== "offerer") throw new Error("offer cannot be started");
        this.started = true;
        this.attach(this.peer.createDataChannel(label, { ordered: true }));
        if (this.status === "failed") return;
        const offer = await this.peer.createOffer();
        if (this.closed || this.status === "failed") return;
        await this.peer.setLocalDescription(offer);
        if (!this.closed && this.status !== "failed") this.emitLocalDescription();
    }
    async setRemoteDescription(description) {
        if (this.closed || this.remoteSet || this.remoteSetting || !description ||
            (this.role === "offerer" && !this.started) ||
            description.type !== (this.role === "offerer" ? "answer" : "offer") ||
            typeof description.sdp !== "string" || description.sdp.length > maxSignalBytes ||
            new TextEncoder().encode(description.sdp).length > maxSignalBytes)
            throw new TypeError("invalid remote description");
        this.remoteSetting = true;
        try {
            await this.peer.setRemoteDescription(description);
            if (this.closed || this.status === "failed") return;
            this.remoteSet = true;
            await this.flushCandidates();
            if (this.closed || this.status === "failed") return;
            if (this.role === "answerer") {
                const answer = await this.peer.createAnswer();
                if (this.closed || this.status === "failed") return;
                await this.peer.setLocalDescription(answer);
                if (!this.closed && this.status !== "failed") this.emitLocalDescription();
            }
        } finally { this.remoteSetting = false; }
    }
    async addRemoteCandidate(candidate) {
        if (this.closed || this.status === "failed" || !candidate || typeof candidate.candidate !== "string")
            throw new TypeError("invalid ICE candidate");
        if (candidate.candidate.length > 4096) throw new RangeError("ICE candidate limits exceeded");
        const mid = candidate.sdpMid ?? null, index = candidate.sdpMLineIndex ?? null;
        const username = candidate.usernameFragment ?? null;
        if ((mid !== null && (typeof mid !== "string" || mid.length > 256)) ||
            (username !== null && (typeof username !== "string" || username.length > 256)) ||
            (index !== null && (!Number.isInteger(index) || index < 0 || index > 65535)) ||
            (mid === null && index === null)) throw new TypeError("invalid ICE candidate metadata");
        const encoder = new TextEncoder();
        const candidateSize = encoder.encode(candidate.candidate).length;
        const size = candidateSize + encoder.encode(mid ?? "").length +
                     encoder.encode(username ?? "").length;
        if (!candidateSize || candidateSize > 4096 || size > 4608 || this.candidateCount >= 64 ||
            size > 65536 - this.candidateBytes)
            throw new RangeError("ICE candidate limits exceeded");
        this.pendingCandidates.push({ candidate: { candidate: candidate.candidate, sdpMid: mid,
                                                   sdpMLineIndex: index, usernameFragment: username }, size });
        this.candidateBytes += size;
        ++this.candidateCount;
        if (this.remoteSet) await this.flushCandidates();
    }
    async flushCandidates() {
        if (this.candidateDrain) {
            await this.candidateDrain;
            if (!this.closed && this.pendingCandidates.length) await this.flushCandidates();
            return;
        }
        const drain = (async () => {
            while (!this.closed && this.pendingCandidates.length) {
                const item = this.pendingCandidates.shift();
                try { await this.peer.addIceCandidate(item.candidate); }
                finally {
                    if (!this.closed && this.status !== "failed") { this.candidateBytes -= item.size; --this.candidateCount; }
                }
            }
        })();
        this.candidateDrain = drain;
        try { await drain; }
        finally { if (this.candidateDrain === drain) this.candidateDrain = null; }
        if (!this.closed && this.pendingCandidates.length) await this.flushCandidates();
    }
}
