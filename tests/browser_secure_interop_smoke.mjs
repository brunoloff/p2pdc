import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { WasmSecureLink } from "../src/browser/wasm_secure_link.mjs";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

const fromHex = (value) => Uint8Array.from(value.match(/../g), (byte) => parseInt(byte, 16));
const toHex = (value) => Array.from(value, (byte) => byte.toString(16).padStart(2, "0")).join("");
const options = { role: "answerer", timeoutMs: 20000,
    identitySeed: new Uint8Array(32).fill(2), recipientSecret: new Uint8Array(32).fill(4),
    capability: new Uint8Array(32).fill(5),
    remoteSigningKey: fromHex("8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c"),
    remoteBoxKey: fromHex("5dfedd3b6bd47f6fa28ee15d969d5bb0ea53774d488bdaf9df1c6e0124b3ef22"),
    localInstance: new Uint8Array(16).fill(2), remoteInstance: new Uint8Array(16).fill(1),
    attemptId: new Uint8Array(16).fill(3) };
const now = () => Math.floor(Date.now() / 1000);
const encoder = new TextEncoder(), decoder = new TextDecoder("utf-8", { fatal: true });
const payload = Uint8Array.from({ length: 70000 }, (_, i) => i % 251);
let transport, link, failure = null, received = false, ready = false, finishing = false, tamperChecked = false;
let signalChain = Promise.resolve();
function post(value) {
    signalChain = signalChain.then(async () => {
        const response = await fetch("/interop-signal", { method: "POST", signal: AbortSignal.timeout(5000),
            headers: { "Content-Type": "application/json" }, body: JSON.stringify(value) });
        if (!response.ok) throw new Error(`signaling rejected: ${response.status}`);
    });
    return signalChain;
}
const transportHandlers = {
    onDescription: (description) => {
        if (link.localDescription(encoder.encode(description.sdp), now()) !== "accepted") failure = new Error("local description failed");
    },
    onCandidate: (candidate) => {
        if (typeof candidate.sdpMid !== "string" || !candidate.sdpMid ||
            link.localCandidate(encoder.encode(candidate.candidate), encoder.encode(candidate.sdpMid), now()) !== "accepted")
            failure = new Error("local candidate failed");
    },
    onState: (state) => {
        if (state === "failed") { if (!finishing) failure = new Error("transport failed"); return; }
        if (!link.setTransportReady(state === "ready", transport.maxMessageSize())) failure = new Error("transport readiness failed");
    },
    onBinary: (frame) => { if (!link.onFrame(frame)) failure = new Error("frame rejected"); },
    onRoute: () => {}, onError: (reason) => { if (!finishing) failure = new Error(reason); }, onWritable: () => link.wake(),
};
const secureHandlers = {
    sendFrame: (frame) => transport.send(frame), bufferedAmount: () => transport.bufferedAmount(),
    onSignal: (wire) => post({ kind: "wire", data: toHex(wire) }),
    onDescription: async (sdp) => transport.setRemoteDescription({ type: "offer", sdp: decoder.decode(sdp) }),
    onCandidate: async (candidate, mid) => transport.addRemoteCandidate({ candidate: decoder.decode(candidate), sdpMid: decoder.decode(mid) }),
    onControl: () => {}, onWritable: () => {}, onFailure: (reason) => { failure = new Error(reason); },
    onReady: () => {
        if (ready || link.send(payload) !== "accepted") { failure = new Error("browser send failed"); return; }
        ready = true;
    },
    onMessage: (bytes) => {
        if (received || bytes.length !== 90000 || !bytes.every((value, i) => value === (i * 7) % 256)) {
            failure = new Error("native message mismatch"); return;
        }
        received = true;
        void post({ kind: "done" }).catch((error) => { failure = error; });
    },
};
async function poll() {
    const response = await fetch("/interop-events", { cache: "no-store", signal: AbortSignal.timeout(5000) });
    if (!response.ok) throw new Error("signaling poll failed");
    const batch = await response.json();
    for (const event of batch.events) {
        if (event.kind !== "wire") throw new Error("unexpected signaling event");
        const wire = fromHex(event.data);
        if (!tamperChecked) {
            const changed = wire.slice(); changed[changed.length - 1] ^= 1;
            if (link.receive(changed, now()) !== "ignored" || ready || received) throw new Error("tampered signaling accepted");
            tamperChecked = true;
        }
        if (link.receive(wire, now()) !== "accepted") throw new Error("native signaling rejected");
    }
    if (failure) throw failure;
    if (batch.result === "exchange") {
        if (!received || !ready || !tamperChecked) throw new Error("incomplete authenticated exchange");
        if (!finishing) { finishing = true; await post({ kind: "stop" }); }
        return false;
    }
    if (batch.result === "pass") {
        if (!finishing || !received || !ready) throw new Error("unexpected native exit");
        return true;
    }
    if (batch.result !== "pending") throw new Error(batch.result);
    return false;
}
try {
    const module = await Promise.race([createModule(), new Promise((_, reject) => setTimeout(() => reject(new Error("module timed out")), 10000))]);
    transport = new BrowserWebRtcTransport({ role: "answerer", relayPolicy: "automatic" }, transportHandlers);
    link = new WasmSecureLink(module, options, secureHandlers);
    const deadline = Date.now() + 20000;
    while (!(await poll())) {
        if (Date.now() > deadline) throw new Error("authenticated interop timed out");
        await new Promise((done) => setTimeout(done, 20));
    }
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { if (transport) transport.close(); if (link) link.close(); }
