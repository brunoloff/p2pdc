// An in-memory stand-in for RTCPeerConnection, since Node has no WebRTC. Like real ICE, two
// connections connect only once each holds the other's description and at least one of its
// candidates, so the whole OFFER, ANSWER and CANDIDATE exchange must happen; and, as in
// browsers, adding a candidate before the remote description throws. The real thing is
// tested in Chromium (peerjs_webrtc_browser_test.mjs).
class FakeChannel {
    constructor(label) { this.label = label; this.readyState = "connecting"; this.bufferedAmount = 0; this.peer = null; this.sent = 0; }
    send(data) {
        if (this.readyState !== "open") throw new DOMException("channel is not open", "InvalidStateError");
        ++this.sent;
        const peer = this.peer;
        setTimeout(() => { if (peer.readyState === "open") peer.onmessage?.({ data }); }, 0);
    }
    open() { this.readyState = "open"; this.onopen?.(); }
    close() {
        if (this.readyState === "closed") return;
        this.readyState = "closed";
        setTimeout(() => this.onclose?.(), 0);
        this.peer?.close();
    }
}

export function fakeWebRtc() {
    const sessions = new Map(), connections = [];
    let next = 0;
    class FakePeerConnection {
        constructor(config) {
            this.config = config; this.id = ++next; this.connectionState = "new";
            this.localDescription = null; this.remoteDescription = null; this.peer = null;
            this.channels = []; this.candidates = 0;
            sessions.set(this.id, this); connections.push(this);
        }
        createDataChannel(label) { const channel = new FakeChannel(label); this.channels.push(channel); return channel; }
        async createOffer() { return { type: "offer", sdp: `v=0\r\no=fake ${this.id}\r\n` }; }
        async createAnswer() {
            if (!this.remoteDescription) throw new DOMException("no offer", "InvalidStateError");
            return { type: "answer", sdp: `v=0\r\no=fake ${this.id}\r\n` };
        }
        async setLocalDescription(description) {
            this.localDescription = { type: description.type, sdp: description.sdp };
            setTimeout(() => {
                if (this.connectionState === "closed") return;
                const candidate = `candidate:1 1 udp 2122260223 127.0.0.1 ${1000 + this.id} typ host`;
                this.onicecandidate?.({ candidate: { candidate, sdpMid: "0", sdpMLineIndex: 0, usernameFragment: "fake" } });
                this.onicecandidate?.({ candidate: null });
            }, 0);
        }
        async setRemoteDescription(description) {
            const peer = sessions.get(Number(/o=fake (\d+)/.exec(description.sdp)?.[1]));
            if (!peer) throw new DOMException("unknown session", "OperationError");
            this.peer = peer; this.remoteDescription = { type: description.type, sdp: description.sdp };
            this.connectIfReady();
        }
        async addIceCandidate(candidate) {
            if (!this.remoteDescription) throw new DOMException("no remote description", "InvalidStateError");
            if (!candidate.candidate.startsWith("candidate:")) throw new DOMException("bad candidate", "OperationError");
            ++this.candidates;
            this.connectIfReady();
        }
        connectIfReady() {
            const peer = this.peer;
            if (peer?.peer !== this || !this.candidates || !peer.candidates || this.connectionState !== "new") return;
            const [offerer, answerer] = this.channels.length ? [this, peer] : [peer, this];
            for (const pc of [offerer, answerer]) { pc.connectionState = "connected"; pc.onconnectionstatechange?.(); }
            setTimeout(() => {
                for (const channel of offerer.channels) {
                    const mirror = new FakeChannel(channel.label);
                    channel.peer = mirror; mirror.peer = channel;
                    answerer.ondatachannel?.({ channel: mirror });
                    mirror.open(); channel.open();
                }
            }, 0);
        }
        // As when the network path dies: both sides see the connection fail.
        breakPath() {
            for (const pc of [this, this.peer]) { if (pc) { pc.connectionState = "failed"; pc.onconnectionstatechange?.(); } }
        }
        close() {
            if (this.connectionState === "closed") return;
            this.connectionState = "closed";
            for (const channel of this.channels) channel.close();
            sessions.delete(this.id);
        }
    }
    return { RTCPeerConnection: FakePeerConnection, connections, iceServers: [] };
}
