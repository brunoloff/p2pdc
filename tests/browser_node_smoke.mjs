import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserNode } from "../src/browser/browser_node.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

const fromHex = (value) => Uint8Array.from(value.match(/../g), (byte) => parseInt(byte, 16));
const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
const capability = crypto.getRandomValues(new Uint8Array(32));
const identity = (index) => ({ identitySeed: new Uint8Array(32).fill(index + 1),
    recipientSecret: new Uint8Array(32).fill(index + 3), capability: capability.slice() });
const expected = [fromHex("8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c"),
                  fromHex("8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394")];
const turn = new URLSearchParams(location.search).get("turn");
const small = new URLSearchParams(location.search).get("small") === "1";
const recovery = new URLSearchParams(location.search).get("recovery") === "pending";
const outage = new URLSearchParams(location.search).get("outage") === "1";
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
let first, second, outgoing, incoming, resolve, reject, requests = 0, ready = 0, recovering = false, phase = 0, degraded = 0;
const received = [false, false], payloads = [Uint8Array.of(0, 1, 255), Uint8Array.of(42, 0)];
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const fail = (reason) => reject(new Error(String(reason)));
const transport = turn === "both" ? { relayPolicy: "requireLocalRelay", iceServers: [{
    urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc", credential: "temporary-test-password" }] } :
    { relayPolicy: "automatic" };
const connectionHandlers = (index) => ({
    onState: (state) => {
        if (state === "ready" && ++ready === 2 &&
            (outgoing.send(payloads[0]) !== "accepted" || incoming.send(payloads[1]) !== "accepted")) fail("send failed");
    },
    onMessage: (message) => {
        if (received[index] || !equal(message, payloads[1 - index])) { fail("payload mismatch"); return; }
        received[index] = true;
        if (received.every(Boolean)) {
            if (!outage || phase === 1) { resolve(); return; }
            phase = 1; received.fill(false);
            outgoing.mailbox.fail(new ProxyError("stream_closed", "injected interruption"));
            incoming.listener.mailbox.fail(new ProxyError("stream_closed", "injected interruption"));
        }
    }, onRoute: () => {}, onWritable: () => {}, onStatus: (status) => {
        if (status === "signalingUnavailable" && ++degraded === 2 &&
            (outgoing.send(payloads[0]) !== "accepted" || incoming.send(payloads[1]) !== "accepted")) fail("send after outage failed");
    }, onError: fail,
});
let timeout;
try {
    const parameters = new URLSearchParams(location.search), port = parameters.get("proxyPort");
    if (turn !== null && turn !== "both") throw new Error("invalid TURN test mode");
    if (port !== null && (!/^\d{1,5}$/.test(port) || Number(port) < 1 || Number(port) > 65535))
        throw new Error("invalid local proxy port");
    const module = await createModule();
    const proxy = port === null ? new MemoryProxy() : new OpenDhtProxy(`http://127.0.0.1:${port}`, { timeoutMs: 5000 });
    first = new BrowserNode(module, proxy, identity(0), {
        onPeer: (peer, event) => {
            if (event === "expired" || outgoing || !equal(peer.signingKey, expected[1])) return;
            outgoing = first.connect(peer, { timeoutMs: 10000, transport }, connectionHandlers(0));
        }, onCatchup: () => {}, onError: fail,
    });
    second = new BrowserNode(module, proxy, identity(1), { onPeer: () => {}, onCatchup: () => {}, onError: fail,
        onStatus: (status) => {
            if (status === "ready" && recovering) {
                recovering = false;
                void incoming.accept().then((ok) => { if (!ok) fail("accept after recovery failed"); }, fail);
            }
        } }, { watch: { recovery: { retryDelayMs: 50 } } });
    if (!(await first.start()) || !(await second.start())) throw new Error("node startup failed");
    incoming = second.listen({ timeoutMs: 10000, transport, maxMessageBytes: small ? 8192 : 262144 },
        { ...connectionHandlers(1), onPublished: () => {},
        onRequest: (peer) => {
            if (++requests !== 1 || !equal(peer.signingKey, expected[0])) { fail("unexpected request"); return; }
            if (recovery) {
                recovering = true;
                second.watch.mailbox.current.fail(new ProxyError("stream_closed", "injected interruption"));
                void incoming.accept().then((ok) => { if (ok || incoming.state !== "pending") fail("request did not wait"); }, fail);
                return;
            }
            void incoming.accept().then((ok) => { if (!ok) fail("accept failed"); }, fail);
        } });
    timeout = setTimeout(() => fail("node exchange timed out"), 12000);
    await complete;
    const expectedLimit = small ? 8192 : 262144;
    if (outgoing.maxMessageSize() !== expectedLimit || incoming.maxMessageSize() !== expectedLimit)
        throw new Error("negotiated message limit mismatch");
    if (outgoing.send(new Uint8Array(expectedLimit + 1)) !== "tooLarge") throw new Error("message limit not enforced");
    if (turn === "both") {
        const routes = await Promise.all([outgoing.pollRoute(), incoming.pollRoute()]);
        if (routes.some((route) => route.current !== "turnRelayed" || route.turnHistory !== "used" ||
            route.selectedPair?.local !== "relayed")) throw new Error("TURN route not observed");
    }
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { clearTimeout(timeout); first?.close(); second?.close(); }
