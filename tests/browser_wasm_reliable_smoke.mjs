import createModule from "../build-wasm/p2pdc_browser_reliable.mjs";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";
import { WasmReliableLink } from "../src/browser/wasm_reliable_link.mjs";

let transports = [], links = [], resolveTest, rejectTest, started = false;
const ready = [false, false], received = [false, false];
const complete = new Promise((resolve, reject) => { resolveTest = resolve; rejectTest = reject; });
const payloads = [Uint8Array.from({ length: 70000 }, (_, i) => i % 251),
                  Uint8Array.from({ length: 90000 }, (_, i) => (i * 7) % 256)];
const safely = (promise) => { void promise.catch(rejectTest); };
const handlers = (index) => ({
    onDescription: (description) => safely(transports[1 - index].setRemoteDescription(description)),
    onCandidate: (candidate) => safely(transports[1 - index].addRemoteCandidate(candidate)),
    onState: (state) => {
        if (state === "failed") { rejectTest(new Error("transport failed")); return; }
        if (state !== "ready") { links[index].setReady(false); return; }
        if (!links[index].setReady(true, transports[index].maxMessageSize())) {
            rejectTest(new Error("transport limit too small")); return;
        }
        ready[index] = true;
        if (ready.every(Boolean) && !started) {
            started = true;
            if (links[0].send(payloads[0]) !== "accepted" || links[1].send(payloads[1]) !== "accepted")
                rejectTest(new Error("message submission failed"));
        }
    },
    onBinary: (bytes) => { if (!links[index].onFrame(bytes)) rejectTest(new Error("frame rejected")); },
    onRoute: () => {}, onError: (error) => rejectTest(new Error(error)),
    onWritable: () => links[index].wake(),
});
const linkHandlers = (index) => ({
    sendFrame: (frame) => transports[index].send(frame),
    bufferedAmount: () => transports[index].bufferedAmount(),
    onMessage: (bytes) => {
        const expected = payloads[1 - index];
        if (bytes.length !== expected.length || !bytes.every((value, i) => value === expected[i]))
            rejectTest(new Error("reassembled message mismatch"));
        received[index] = true;
        if (received.every(Boolean)) resolveTest();
    },
    onFailure: (error) => rejectTest(new Error(error)), onWritable: () => {},
});

try {
    const module = await createModule();
    transports = [new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "automatic" }, handlers(0)),
                  new BrowserWebRtcTransport({ role: "answerer", relayPolicy: "automatic" }, handlers(1))];
    links = [new WasmReliableLink(module, linkHandlers(0)), new WasmReliableLink(module, linkHandlers(1))];
    const timeout = setTimeout(() => rejectTest(new Error("connection timed out")), 10000);
    await transports[0].startOffer();
    await complete;
    clearTimeout(timeout);
    document.body.dataset.result = "pass";
    document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail";
    document.body.textContent = `fail: ${error.message}`;
} finally {
    for (const transport of transports) transport.close();
    for (const link of links) link.close();
}
