import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserConnection } from "../src/browser/browser_connection.mjs";
import { BrowserListener } from "../src/browser/browser_listener.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { ProxyOutbox } from "../src/browser/proxy_outbox.mjs";

const identity = { identitySeed: new Uint8Array(32).fill(2), recipientSecret: new Uint8Array(32).fill(4),
    capability: new Uint8Array(32).fill(5) };
const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
let watch, listener, connection, outbox, nativePeer, resolve, reject, failure = null, requestCount = 0;
let ready = false, finishing = false;
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const fail = (reason) => { failure ??= new Error(String(reason)); reject(failure); };
const connectionHandlers = {
    onSignal: (wire) => { if (outbox.send(wire) !== "accepted") throw new Error("signaling queue failed"); },
    onState: (state) => {
        if (state !== "ready") return;
        if (ready || connection.send(Uint8Array.of(0, 1, 255)) !== "accepted") fail("browser send failed");
        ready = true;
    },
    onMessage: (message) => {
        if (!ready || !equal(message, Uint8Array.of(42, 0))) { fail("native message mismatch"); return; }
        if (connection.send(Uint8Array.of(7)) !== "accepted") { fail("acknowledgement failed"); return; }
        resolve();
    },
    onRoute: () => {}, onWritable: () => {}, onError: (reason) => { if (!finishing) fail(reason); },
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
async function accept(module, proxy, peer) {
    if (++requestCount !== 1 || !nativePeer || peer.handle !== nativePeer.handle ||
        !equal(peer.signingKey, nativePeer.signingKey)) throw new Error("unexpected request");
    const offer = listener.accept((wire) => {
        const result = connection.receive(wire);
        if (result !== "accepted" && result !== "ignored") throw new Error("signaling record failed");
    });
    if (!offer) throw new Error("accept failed");
    outbox = new ProxyOutbox(proxy, offer.peer.inbox, { onPublished: () => {}, onError: fail });
    connection = new BrowserConnection(module, {
        secure: { ...identity, role: "answerer", timeoutMs: 20000, localInstance: offer.local.instance,
            remoteInstance: offer.peer.instance, remoteSigningKey: offer.peer.signingKey,
            remoteBoxKey: offer.peer.recipientKey, attemptId: offer.attemptId },
        transport: { role: "answerer", relayPolicy: "automatic" },
    }, connectionHandlers);
    await connection.start();
    if (connection.receive(offer.offer) !== "accepted" || !listener.resume()) throw new Error("offer handoff failed");
}
let timeout;
try {
    const module = await createModule(), proxy = new OpenDhtProxy("http://127.0.0.1:49412", { timeoutMs: 5000 });
    const discovery = { windowed: new URLSearchParams(location.search).get("windowed") !== "0" };
    watch = new BrowserWatch(module, proxy, identity, {
        onPeer: (peer, event) => {
            if (event !== "expired" && listener && !equal(peer.signingKey, listener.lease.endpoint().signingKey))
                nativePeer = peer;
            listener?.retry();
        }, onCatchup: () => listener?.retry(), onError: fail,
    }, discovery);
    listener = new BrowserListener(module, proxy, identity, watch, {
        onPublished: () => {}, onRequest: (peer) => { void accept(module, proxy, peer).catch(fail); },
        onStatus: () => {}, onError: fail,
    }, discovery);
    if (!(await watch.start()) || !(await listener.start())) throw new Error("browser startup failed");
    timeout = setTimeout(() => fail("browser exchange timed out"), 25000);
    await complete;
    await finishNative();
    if (failure) throw failure;
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { clearTimeout(timeout); connection?.close(); outbox?.close(); listener?.close(); watch?.close(); }
