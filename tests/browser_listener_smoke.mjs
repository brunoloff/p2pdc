import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserConnection } from "../src/browser/browser_connection.mjs";
import { BrowserOutgoing } from "../src/browser/browser_outgoing.mjs";
import { BrowserIncoming } from "../src/browser/browser_incoming.mjs";
import { BrowserLease } from "../src/browser/browser_lease.mjs";
import { BrowserListener } from "../src/browser/browser_listener.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";
import { ProxyMailbox } from "../src/browser/proxy_mailbox.mjs";
import { ProxyOutbox } from "../src/browser/proxy_outbox.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";

const capability = crypto.getRandomValues(new Uint8Array(32));
const identity = (index) => ({ identitySeed: new Uint8Array(32).fill(index + 1),
    recipientSecret: new Uint8Array(32).fill(index + 3), capability: capability.slice() });
const equal = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const turn = new URLSearchParams(location.search).get("turn");
const outgoing = new URLSearchParams(location.search).get("outgoing");
const incoming = new URLSearchParams(location.search).get("incoming");
class MemoryProxy {
    constructor() { this.records = new Map(); this.listeners = new Map(); }
    key(bytes) { return [...bytes].join(","); }
    async put(key, bytes) {
        const id = this.key(key), record = { bytes: bytes.slice(), expired: false, id: null };
        if (!this.records.has(id)) this.records.set(id, []);
        this.records.get(id).push(record);
        for (const callback of this.listeners.get(id) ?? []) queueMicrotask(() => callback(record));
    }
    async get(key) { return this.records.get(this.key(key)) ?? []; }
    listen(key, callback) {
        const id = this.key(key), callbacks = this.listeners.get(id) ?? new Set();
        callbacks.add(callback); this.listeners.set(id, callbacks);
        let finish;
        const done = new Promise((resolve) => { finish = resolve; });
        return { ready: Promise.resolve(true), done, close: () => { callbacks.delete(callback); finish(); } };
    }
}
let listener, senderLease, senderWatch, receiverWatch, senderMailbox, senderOutbox, receiverOutbox;
let sender, receiver, remotePeer, resolve, reject, ready = 0, requests = 0, dialing = false;
const received = [false, false], payloads = [Uint8Array.of(0, 1, 255), Uint8Array.of(42, 0)];
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const report = (reason) => reject(new Error(String(reason)));
const sendSignal = (outbox, wire) => { if (outbox.send(wire) !== "accepted") throw new Error("outbox admission failed"); };
const connectionOptions = (index, local, peer, attemptId) => {
    const role = index ? "answerer" : "offerer";
    return { secure: { ...identity(index), role, timeoutMs: 10000, localInstance: local.instance,
        remoteInstance: peer.instance, remoteSigningKey: peer.signingKey, remoteBoxKey: peer.recipientKey, attemptId },
        transport: turn === "both" ? { role, relayPolicy: "requireLocalRelay",
            iceServers: [{ urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc",
                credential: "temporary-test-password" }] } : { role, relayPolicy: "automatic" } };
};
const connectionHandlers = (index) => ({
    onSignal: (wire) => sendSignal(index ? receiverOutbox : senderOutbox, wire),
    onState: (state) => {
        if (state === "ready" && ++ready === 2 &&
            (sender.send(payloads[0]) !== "accepted" || receiver.send(payloads[1]) !== "accepted")) report("send failed");
    },
    onMessage: (value) => {
        if (!equal(value, payloads[1 - index]) || received[index]) { report("payload mismatch"); return; }
        received[index] = true; if (received.every(Boolean)) resolve();
    },
    onRoute: () => {}, onWritable: () => {}, onStatus: () => {}, onError: report,
});
function dial(module, proxy) {
    if (dialing || !remotePeer || (outgoing !== "owner" && !senderLease.self())) return;
    const current = senderWatch.current(remotePeer.handle);
    const expected = (incoming === "owner" ? listener.listener.lease : listener.lease).endpoint();
    if (!current || !expected || !equal(current.signingKey, expected.signingKey) ||
        !equal(current.recipientKey, expected.recipientKey) || !equal(current.instance, expected.instance) ||
        !equal(current.inbox, expected.inbox)) return;
    remotePeer = current;
    dialing = true;
    if (outgoing === "owner") {
        const transport = turn === "both" ? { relayPolicy: "requireLocalRelay",
            iceServers: [{ urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc",
                credential: "temporary-test-password" }] } : { relayPolicy: "automatic" };
        sender = new BrowserOutgoing(module, proxy, identity(0), senderWatch, remotePeer,
            { timeoutMs: 10000, transport }, connectionHandlers(0));
        void sender.start().then((ok) => { if (!ok) report("outgoing startup failed"); }, report);
        return;
    }
    const local = senderLease.self(), attemptId = crypto.getRandomValues(new Uint8Array(16));
    senderOutbox = new ProxyOutbox(proxy, remotePeer.inbox, { onPublished: () => {}, onError: report });
    sender = new BrowserConnection(module, connectionOptions(0, local, remotePeer, attemptId), connectionHandlers(0));
    void sender.start().catch(report);
}
async function accept(module, proxy) {
    const accepted = listener.accept((wire) => {
        const result = receiver.receive(wire);
        if (result !== "accepted" && result !== "ignored") throw new Error("incoming record failed");
    });
    if (!accepted) throw new Error("consent failed");
    receiverOutbox = new ProxyOutbox(proxy, accepted.peer.inbox, { onPublished: () => {}, onError: report });
    receiver = new BrowserConnection(module, connectionOptions(1, accepted.local, accepted.peer, accepted.attemptId),
        connectionHandlers(1));
    await receiver.start();
    if (receiver.receive(accepted.offer) !== "accepted" || !listener.resume()) throw new Error("offer handoff failed");
}
let timeout;
try {
    const port = new URLSearchParams(location.search).get("proxyPort");
    if (turn !== null && turn !== "both") throw new Error("invalid TURN test mode");
    if (outgoing !== null && outgoing !== "owner") throw new Error("invalid outgoing test mode");
    if (incoming !== null && incoming !== "owner") throw new Error("invalid incoming test mode");
    if (port !== null && (!/^\d{1,5}$/.test(port) || Number(port) < 1 || Number(port) > 65535))
        throw new Error("invalid local proxy port");
    const module = await createModule();
    const proxy = port === null ? new MemoryProxy() : new OpenDhtProxy(`http://127.0.0.1:${port}`, { timeoutMs: 5000 });
    senderLease = new BrowserLease(module, proxy, identity(0), { onPublished: () => dial(module, proxy), onError: report });
    const senderEndpoint = senderLease.endpoint();
    senderMailbox = new ProxyMailbox(proxy, senderEndpoint.inbox, {
        onRecord: (wire) => { const result = sender?.receive(wire); if (result !== "accepted" && result !== "ignored") report("sender record failed"); },
        onCatchup: () => {}, onError: report,
    });
    senderWatch = new BrowserWatch(module, proxy, identity(0), {
        onPeer: (peer, event) => { if (event !== "expired" && !equal(peer.signingKey, senderEndpoint.signingKey)) {
            remotePeer = peer; dial(module, proxy);
        } }, onCatchup: () => {}, onError: report,
    });
    receiverWatch = new BrowserWatch(module, proxy, identity(1), {
        onPeer: () => listener?.retry(), onCatchup: () => listener?.retry(), onError: report,
    });
    const requestHandlers = {
        onPublished: () => {}, onRequest: (peer) => {
            const expected = outgoing === "owner" ? sender.lease.endpoint() : senderEndpoint;
            if (++requests !== 1 || !equal(peer.signingKey, expected.signingKey) ||
                !equal(peer.recipientKey, expected.recipientKey) || !equal(peer.instance, expected.instance) ||
                !equal(peer.inbox, expected.inbox)) { report("unexpected consent request"); return; }
            if (incoming === "owner") {
                receiver = listener;
                void listener.accept().then((ok) => { if (!ok) report("accept failed"); }, report);
            } else void accept(module, proxy).catch(report);
        },
        onStatus: () => {}, onError: report,
    };
    if (incoming === "owner") {
        const transport = turn === "both" ? { relayPolicy: "requireLocalRelay",
            iceServers: [{ urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc",
                credential: "temporary-test-password" }] } : { relayPolicy: "automatic" };
        listener = new BrowserIncoming(module, proxy, identity(1), receiverWatch,
            { timeoutMs: 10000, transport }, { ...connectionHandlers(1), ...requestHandlers });
    } else listener = new BrowserListener(module, proxy, identity(1), receiverWatch, requestHandlers);
    if ((outgoing !== "owner" && !(await senderMailbox.start())) || !(await senderWatch.start()) || !(await receiverWatch.start()))
        throw new Error("discovery startup failed");
    if ((outgoing !== "owner" && !senderLease.start()) || !(await listener.start())) throw new Error("announcement startup failed");
    timeout = setTimeout(() => report("listener test timed out"), 12000);
    await complete;
    if (turn === "both") {
        const routes = await Promise.all([sender.pollRoute(), receiver.pollRoute()]);
        if (routes.some((route) => route.current !== "turnRelayed" || route.turnHistory !== "used" ||
            route.selectedPair?.local !== "relayed")) throw new Error("TURN route not observed");
    }
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally {
    clearTimeout(timeout);
    sender?.close(); receiver?.close(); senderOutbox?.close(); receiverOutbox?.close(); senderMailbox?.close();
    listener?.close(); senderLease?.close(); senderWatch?.close(); receiverWatch?.close();
}
