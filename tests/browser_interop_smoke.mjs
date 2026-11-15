import createModule from "../build-wasm/p2pdc_browser_reliable.mjs";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";
import { WasmReliableLink } from "../src/browser/wasm_reliable_link.mjs";

let transport, link, signalChain = Promise.resolve(), received = false, ready = false, finishing = false;
let failure = null;
const payload = Uint8Array.from({ length: 70000 }, (_, i) => i % 251);
const post = (message) => {
    signalChain = signalChain.then(async () => {
        const response = await fetch("/interop-signal", { method: "POST", signal: AbortSignal.timeout(5000),
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(message) });
        if (!response.ok) throw new Error(`signaling rejected: ${response.status}`);
    }).catch((error) => { failure = error; });
};
const transportHandlers = {
    onDescription: (d) => post({ kind: "description", type: d.type, sdp: d.sdp }),
    onCandidate: (c) => post({ kind: "candidate", candidate: c.candidate, sdpMid: c.sdpMid }),
    onState: (state) => {
        if (state === "failed") { if (!finishing) failure = new Error("transport failed"); return; }
        if (state !== "ready") { link.setReady(false); return; }
        if (!link.setReady(true, transport.maxMessageSize())) { failure = new Error("unsupported frame size"); return; }
        if (!ready && link.send(payload) !== "accepted") failure = new Error("browser send failed");
        ready = true;
    },
    onBinary: (bytes) => { if (!link.onFrame(bytes)) failure = new Error("frame rejected"); },
    onRoute: () => {}, onError: (error) => { if (!finishing) failure = new Error(error); },
    onWritable: () => link.wake(),
};
const linkHandlers = {
    sendFrame: (frame) => transport.send(frame), bufferedAmount: () => transport.bufferedAmount(),
    onMessage: (bytes) => {
        if (received || bytes.length !== 90000 || !bytes.every((v, i) => v === (i * 7) % 256)) {
            failure = new Error("native message mismatch"); return;
        }
        received = true;
        post({ kind: "done" });
    },
    onFailure: (reason) => { failure = new Error(reason); }, onWritable: () => {},
};
async function poll() {
    const response = await fetch("/interop-events", { cache: "no-store", signal: AbortSignal.timeout(5000) });
    if (!response.ok) throw new Error("signaling poll failed");
    const batch = await response.json();
    for (const event of batch.events) {
        if (event.kind === "description") await transport.setRemoteDescription(event);
        else if (event.kind === "candidate") await transport.addRemoteCandidate(event);
        else throw new Error("invalid signaling event");
    }
    if (failure) throw failure;
    if (batch.result === "exchange") {
        if (!received || !ready) throw new Error("incomplete exchange");
        if (!finishing) {
            finishing = true;
            post({ kind: "stop" });
            await signalChain;
            if (failure) throw failure;
        }
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
    const module = await Promise.race([
        createModule(),
        new Promise((_, reject) => setTimeout(() => reject(new Error("module initialization timed out")), 10000)),
    ]);
    transport = new BrowserWebRtcTransport({ role: "answerer", relayPolicy: "automatic" }, transportHandlers);
    link = new WasmReliableLink(module, linkHandlers);
    const deadline = Date.now() + 20000;
    while (!(await poll())) {
        if (Date.now() > deadline) throw new Error("interop timed out");
        await new Promise((resolve) => setTimeout(resolve, 20));
    }
    document.body.dataset.result = "pass";
    document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail";
    document.body.textContent = `fail: ${error.message}`;
} finally {
    if (transport) transport.close();
    if (link) link.close();
}
