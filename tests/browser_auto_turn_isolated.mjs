import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

const mode = new URLSearchParams(location.search).get("mode");
const delayed = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const requireValue = (value, message) => { if (!value) throw new Error(message); };
let a, b, failure = null, sent = false;
const ready = [false, false], received = [false, false], hostCandidates = [false, false];
const candidates = [[], []];
const safely = (promise) => { void promise.catch((error) => { failure ??= error; }); };
const handlers = (index) => ({
    onDescription: (value) => safely((index === 0 ? b : a).setRemoteDescription(value)),
    onCandidate: (value) => {
        candidates[index].push(value.candidate);
        if (/\btyp host\b/.test(value.candidate)) hostCandidates[index] = true;
        safely((index === 0 ? b : a).addRemoteCandidate(value));
    },
    onState: (state) => {
        if (state !== "ready") return;
        ready[index] = true;
        if (mode === "turn" && ready.every(Boolean) && !sent) {
            sent = true;
            if (a.send(Uint8Array.of(0, 255)) !== "accepted" || b.send(Uint8Array.of(42)) !== "accepted")
                failure = new Error("send rejected");
        }
    },
    onBinary: (bytes) => {
        const expected = index === 0 ? [42] : [0, 255];
        if (bytes.length !== expected.length || !bytes.every((value, i) => value === expected[i]))
            failure = new Error("binary mismatch");
        received[index] = true;
    },
    onRoute: () => {}, onError: (error) => { if (mode === "turn") failure ??= new Error(error); },
    onWritable: () => {},
});

async function waitFor(condition, ms) {
    const deadline = Date.now() + ms;
    while (Date.now() < deadline) {
        if (failure) throw failure;
        if (condition()) return true;
        await delayed(50);
    }
    return false;
}
async function relayRoutes() {
    for (let attempt = 0; attempt < 50; ++attempt) {
        const routes = await Promise.all([a.pollRoute(), b.pollRoute()]);
        if (routes.every((route) => route.current === "turnRelayed")) return routes;
        await delayed(100);
    }
    throw new Error("TURN route not reported");
}

try {
    requireValue(["blocked", "turn", "bad"].includes(mode), "invalid mode");
    const iceServers = mode === "blocked" ? [] : [{ urls: "turn:10.44.0.1:34789?transport=tcp",
        username: "p2pdc", credential: mode === "bad" ? "wrong-password" : "temporary-test-password" }];
    const options = { relayPolicy: "automatic", iceServers };
    a = new BrowserWebRtcTransport({ ...options, role: "offerer" }, handlers(0));
    b = new BrowserWebRtcTransport({ ...options, role: "answerer" }, handlers(1));
    await a.startOffer();
    if (mode !== "turn") {
        requireValue(!await waitFor(() => ready.some(Boolean), 6000), "direct connection unexpectedly succeeded");
        requireValue(hostCandidates.every(Boolean), `host candidates were not gathered: ${JSON.stringify(candidates)}`);
    } else {
        requireValue(await waitFor(() => received.every(Boolean), 20000), "TURN exchange timed out");
        const routes = await relayRoutes();
        requireValue(routes.every((route) => route.turnHistory === "used"), "TURN history not reported");
    }
    document.body.dataset.result = "pass"; document.body.textContent = `pass: ${mode}`;
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { a?.close(); b?.close(); }
