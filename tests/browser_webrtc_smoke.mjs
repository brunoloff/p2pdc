import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";

let a, b, resolveTest, rejectTest, started = false;
const turn = new URLSearchParams(location.search).get("turn");
const useTurn = turn === "1" || turn === "one" || turn === "auto";
const relay = {
    relayPolicy: "requireLocalRelay",
    iceServers: [{ urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc",
                   credential: "temporary-test-password" }],
};
const ready = [false, false], received = [false, false];
const complete = new Promise((resolve, reject) => { resolveTest = resolve; rejectTest = reject; });
const safely = (promise) => { void promise.catch(rejectTest); };
const handlers = (index) => ({
    onDescription: (description) => safely((index === 0 ? b : a).setRemoteDescription(description)),
    onCandidate: (candidate) => safely((index === 0 ? b : a).addRemoteCandidate(candidate)),
    onState: (state) => {
        if (state === "failed") rejectTest(new Error("transport failed"));
        if (state !== "ready") return;
        ready[index] = true;
        if (ready.every(Boolean) && !started) {
            started = true;
            if (a.send(Uint8Array.of(0, 255)) !== "accepted" || b.send(Uint8Array.of(42)) !== "accepted")
                rejectTest(new Error("send rejected"));
        }
    },
    onBinary: (bytes) => {
        const expected = index === 0 ? [42] : [0, 255];
        if (bytes.length !== expected.length || !bytes.every((value, i) => value === expected[i]))
            rejectTest(new Error("binary mismatch"));
        received[index] = true;
        if (received.every(Boolean)) resolveTest();
    },
    onRoute: () => {}, onError: (error) => rejectTest(new Error(error)), onWritable: () => {},
});

try {
    const offerer = turn === "1" ? relay : { relayPolicy: "automatic", ...(turn === "auto" ? { iceServers: relay.iceServers } : {}) };
    a = new BrowserWebRtcTransport({ role: "offerer", ...offerer }, handlers(0));
    b = new BrowserWebRtcTransport({ role: "answerer", ...(useTurn ? relay : { relayPolicy: "automatic" }) }, handlers(1));
    const timeout = setTimeout(() => rejectTest(new Error("connection timed out")), 8000);
    await a.startOffer();
    await complete;
    const routes = await Promise.all([a.pollRoute(), b.pollRoute()]);
    const expected = useTurn ? "turnRelayed" : "direct";
    if (routes.some((route) => route.current !== expected) ||
        (useTurn && routes[1].selectedPair?.local !== "relayed") ||
        (turn === "1" && routes[0].selectedPair?.local !== "relayed") ||
        ((turn === "one" || turn === "auto") && routes[0].selectedPair?.remote !== "relayed"))
        throw new Error("expected route not observed");
    clearTimeout(timeout);
    document.body.dataset.result = "pass";
    document.body.textContent = `pass: ${JSON.stringify(routes.map((route) => route.selectedPair))}`;
} catch (error) {
    document.body.dataset.result = "fail";
    document.body.textContent = `fail: ${error.message}`;
} finally {
    a?.close(); b?.close();
}
