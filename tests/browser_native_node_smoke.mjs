import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserNode } from "../src/browser/browser_node.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";

const identity = { identitySeed: new Uint8Array(32).fill(2), recipientSecret: new Uint8Array(32).fill(4),
    capability: new Uint8Array(32).fill(5) };
const expected = Uint8Array.from("8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c".match(/../g),
    (value) => parseInt(value, 16));
const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
const role = new URLSearchParams(location.search).get("role");
const parameters = new URLSearchParams(location.search);
const browserSmall = parameters.get("browserSmall") === "1";
const large = parameters.get("large") === "1";
const expectedLimit = parameters.get("small") === "1" || browserSmall ? 8192 : large ? 4194304 : 262144;
const payload = large ? Uint8Array.from({ length: expectedLimit }, (_, i) => i % 251) : null;
let node, connection, resolve, reject, failure = null, requests = 0, ready = false, finishing = false;
let pendingAck = false, pendingPayload = false;
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const fail = (reason) => { failure ??= new Error(String(reason)); reject(failure); };
function sendPending() {
    if (pendingPayload) {
        const status = connection.send(payload ?? Uint8Array.of(0, 1, 255));
        if (status === "accepted") pendingPayload = false;
        else if (status === "backpressure") return;
        else { fail(`browser send failed: ${status}`); return; }
    }
    if (!pendingAck) return;
    const status = connection.send(Uint8Array.of(7));
    if (status === "accepted") { pendingAck = false; resolve(); }
    else if (status !== "backpressure") fail(`acknowledgement failed: ${status}`);
}
const handlers = {
    onState: (state) => {
        if (state !== "ready") return;
        if (connection.maxMessageSize() !== expectedLimit) { fail("negotiated limit mismatch"); return; }
        if ((parameters.get("small") === "1" || browserSmall) &&
            connection.send(new Uint8Array(expectedLimit + 1)) !== "tooLarge") {
            fail("oversized message accepted"); return;
        }
        if (ready) { fail("duplicate ready event"); return; }
        ready = true; pendingPayload = true; sendPending();
    },
    onMessage: (message) => {
        if (!ready || !equal(message, payload ?? Uint8Array.of(42, 0))) { fail("native message mismatch"); return; }
        pendingAck = true; sendPending();
    },
    onRoute: () => {}, onWritable: sendPending, onStatus: () => {},
    onError: (reason) => { if (!finishing) fail(reason); },
};
async function finishNative() {
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
        const response = await fetch("/interop-events", { cache: "no-store", signal: AbortSignal.timeout(5000) });
        if (!response.ok) throw new Error("native status unavailable");
        const result = (await response.json()).result;
        if (result === "exchange" && !finishing) {
            finishing = true;
            const stop = await fetch("/interop-signal", { method: "POST", signal: AbortSignal.timeout(5000),
                headers: { "Content-Type": "application/json" }, body: JSON.stringify({ kind: "stop" }) });
            if (!stop.ok) throw new Error("native stop failed");
        } else if (result === "pass") {
            if (!finishing) throw new Error("unexpected native exit");
            return;
        } else if (result !== "pending" && result !== "exchange") throw new Error(result);
        if (failure) throw failure;
        await new Promise((done) => setTimeout(done, 20));
    }
    throw new Error("native completion timed out");
}
const options = { timeoutMs: 20000, transport: { relayPolicy: "automatic" },
    maxMessageBytes: browserSmall ? 8192 : large || parameters.get("browserLarge") === "1" ? 4194304 : 262144 };
let timeout;
try {
    if (!["incoming", "outgoing"].includes(role)) throw new Error("invalid test role");
    const module = await createModule(), proxy = new OpenDhtProxy("http://127.0.0.1:49412", { timeoutMs: 5000 });
    node = new BrowserNode(module, proxy, identity, {
        onPeer: (peer, event) => {
            if (role !== "outgoing" || event === "expired" || connection || !equal(peer.signingKey, expected)) return;
            connection = node.connect(peer, options, handlers);
        }, onCatchup: () => {}, onError: fail,
    }, parameters.has("windowed") ? { watch: { windowed: parameters.get("windowed") !== "0" } } : {});
    if (!(await node.start())) throw new Error("browser node startup failed");
    if (role === "incoming") {
        connection = node.listen(options, { ...handlers, onPublished: () => {}, onRequest: (peer) => {
            if (++requests !== 1 || !equal(peer.signingKey, expected)) { fail("unexpected request"); return; }
            void connection.accept().then((ok) => { if (!ok) fail("accept failed"); }, fail);
        } });
    }
    timeout = setTimeout(() => fail("browser exchange timed out"), large ? 60000 : 25000);
    await complete; await finishNative();
    if (failure) throw failure;
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { clearTimeout(timeout); node?.close(); }
