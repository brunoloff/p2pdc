import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { WasmSecureLink } from "../src/browser/wasm_secure_link.mjs";
import { BrowserWebRtcTransport } from "../src/browser/webrtc_transport.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { ProxyMailbox } from "../src/browser/proxy_mailbox.mjs";
import { ProxyOutbox } from "../src/browser/proxy_outbox.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";
import { BrowserLease } from "../src/browser/browser_lease.mjs";

const discoveryOptions = (index) => ({
    identitySeed: new Uint8Array(32).fill(index + 1), recipientSecret: new Uint8Array(32).fill(index + 3),
    capability: new Uint8Array(32).fill(5) });
const options = (index, self, peer, attemptId) => ({ ...discoveryOptions(index),
    role: index ? "answerer" : "offerer", timeoutMs: 15000, remoteSigningKey: peer.signingKey,
    remoteBoxKey: peer.recipientKey, localInstance: self.instance, remoteInstance: peer.instance, attemptId });
const now = () => Math.floor(Date.now() / 1000);
const encoder = new TextEncoder(), decoder = new TextDecoder("utf-8", { fatal: true });
const payloads = [Uint8Array.from({ length: 7000 }, (_, i) => i % 251),
                  Uint8Array.from({ length: 9000 }, (_, i) => (i * 7) % 256)];
let links = [], transports = [], mailboxes = [], outboxes = [], watchers = [], leases = [];
let resolve, reject, ready = 0, mailboxError;
const diagnostics = [0, 1].map(() => ({ descriptions: 0, candidates: [], states: [], remoteDescriptions: 0,
    remoteCandidates: 0, appliedCandidates: 0, iceErrors: [] }));
const received = [false, false];
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
void complete.catch((error) => { mailboxError ??= error; });
const same = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
const sameEndpoint = (a, b) => same(a.signingKey, b.signingKey) && same(a.recipientKey, b.recipientKey) &&
    same(a.instance, b.instance) && same(a.inbox, b.inbox);
function transportOptions(index, turn) {
    const role = index ? "answerer" : "offerer";
    if (!turn || (turn === "one" && index === 0)) return { role, relayPolicy: "automatic" };
    return { role, relayPolicy: turn === "both" || index === 1 ? "requireLocalRelay" : "automatic",
        iceServers: [{ urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc",
            credential: "temporary-test-password" }] };
}
async function verifyRoutes(turn) {
    if (!turn) return;
    const routes = await Promise.all(transports.map((transport) => transport.pollRoute()));
    if (routes.some((route) => route.current !== "turnRelayed" || route.turnHistory !== "used"))
        throw new Error("TURN route was not reported by both peers");
    if (routes[1].selectedPair?.local !== "relayed" ||
        (turn === "both" && routes[0].selectedPair?.local !== "relayed") ||
        (turn === "auto" && routes[0].selectedPair?.remote !== "relayed") ||
        (turn === "one" && (routes[0].selectedPair?.remote !== "relayed" ||
            !["host", "serverReflexive", "peerReflexive"].includes(routes[0].selectedPair?.local))))
        throw new Error("unexpected selected TURN candidate pair");
}
async function discoverPeers(module, proxy) {
    const peers = [null, null], selves = [];
    let finish, publishedCount = 0, publicationsDone;
    const found = new Promise((done) => { finish = done; });
    const published = new Promise((done) => { publicationsDone = done; });
    leases = [0, 1].map((index) => new BrowserLease(module, proxy, discoveryOptions(index), {
        onPublished: () => { if (++publishedCount === 2) publicationsDone(); },
        onError: (error) => reject(new Error(error)),
    }));
    selves.push(...leases.map((item) => item.endpoint()));
    mailboxes = selves.map((self, index) => new ProxyMailbox(proxy, self.inbox, {
        onRecord: (wire) => {
            if (!links[index]) { reject(new Error("incoming signaling before consent")); return; }
            const result = links[index].receive(wire, now());
            if (result !== "accepted" && result !== "ignored") reject(new Error("mailbox signaling failed"));
        }, onCatchup: () => {}, onError: (error) => { mailboxError = error; reject(error); },
    }));
    if (!(await Promise.all(mailboxes.map((mailbox) => mailbox.start()))).every(Boolean)) throw new Error("inbox startup failed");
    watchers = [0, 1].map((index) => new BrowserWatch(module, proxy, discoveryOptions(index), {
        onPeer: (peer, event) => {
            if (event === "expired" || !sameEndpoint(peer, selves[1 - index])) return;
            peers[index] = peer;
            if (peers.every(Boolean)) finish();
        }, onCatchup: () => {}, onError: (error) => reject(new Error(error)),
    }));
    if (!(await Promise.all(watchers.map((watcher) => watcher.start()))).every(Boolean)) throw new Error("discovery watch failed");
    for (const lease of leases) if (!lease.start()) throw new Error("advertisement failed");
    let timeout;
    try {
        await Promise.race([Promise.all([found, published]), complete,
            new Promise((_, no) => { timeout = setTimeout(() => no(new Error("discovery timed out")), 10000); })]);
    } finally { clearTimeout(timeout); }
    const verified = peers.map((peer, index) => watchers[index].current(peer.handle));
    if (!verified.every((peer, index) => peer && sameEndpoint(peer, selves[1 - index])))
        throw new Error("discovered peer became stale or unauthorized");
    return { selves, peers: verified };
}
const transportHandlers = (index) => ({
    onDescription: (description) => {
        ++diagnostics[index].descriptions;
        if (links[index].localDescription(encoder.encode(description.sdp), now()) !== "accepted")
            reject(new Error("local description failed"));
    },
    onCandidate: (candidate) => {
        diagnostics[index].candidates.push(candidate.candidate.match(/\btyp (host|srflx|prflx|relay)\b/)?.[1] ?? "unknown");
        if (typeof candidate.sdpMid !== "string" || !candidate.sdpMid ||
            links[index].localCandidate(encoder.encode(candidate.candidate), encoder.encode(candidate.sdpMid), now()) !== "accepted")
            reject(new Error("local candidate failed"));
    },
    onState: (state) => {
        diagnostics[index].states.push(state);
        if (state === "failed") { reject(new Error("transport failed")); return; }
        if (!links[index].setTransportReady(state === "ready", transports[index].maxMessageSize()))
            reject(new Error("transport readiness failed"));
    },
    onBinary: (frame) => { if (!links[index].onFrame(frame)) reject(new Error("frame rejected")); },
    onRoute: () => {}, onError: (reason) => reject(new Error(reason)), onWritable: () => links[index].wake(),
});
const secureHandlers = (index) => ({
    sendFrame: (frame) => transports[index].send(frame), bufferedAmount: () => transports[index].bufferedAmount(),
    onSignal: (wire) => { if (outboxes[index].send(wire) !== "accepted") throw new Error("publication queue failed"); },
    onDescription: async (sdp) => {
        ++diagnostics[index].remoteDescriptions;
        await transports[index].setRemoteDescription({ type: index ? "offer" : "answer", sdp: decoder.decode(sdp) });
    },
    onCandidate: async (candidate, mid) => {
        ++diagnostics[index].remoteCandidates;
        await transports[index].addRemoteCandidate({ candidate: decoder.decode(candidate), sdpMid: decoder.decode(mid) });
        ++diagnostics[index].appliedCandidates;
    },
    onControl: () => {}, onWritable: () => {}, onFailure: (reason) => reject(new Error(reason)),
    onReady: () => {
        if (++ready === 2 && (links[0].send(payloads[0]) !== "accepted" || links[1].send(payloads[1]) !== "accepted"))
            reject(new Error("application send failed"));
    },
    onMessage: (value) => {
        const expected = payloads[1 - index];
        if (received[index] || value.length !== expected.length || !value.every((byte, i) => byte === expected[i]))
            reject(new Error("application message mismatch"));
        received[index] = true;
        if (received.every(Boolean)) resolve();
    },
});
try {
    const parameters = new URLSearchParams(location.search);
    const proxyPort = parameters.get("proxyPort"), turn = parameters.get("turn");
    if (!/^\d{1,5}$/.test(proxyPort ?? "")) throw new Error("local proxy port required");
    if (turn !== null && !["both", "one", "auto"].includes(turn)) throw new Error("invalid TURN test mode");
    const proxy = new OpenDhtProxy(`http://127.0.0.1:${proxyPort}`, { timeoutMs: 5000 });
    const module = await createModule();
    const { selves, peers } = await discoverPeers(module, proxy);
    const attemptId = crypto.getRandomValues(new Uint8Array(16));
    transports = [0, 1].map((index) => new BrowserWebRtcTransport(transportOptions(index, turn), transportHandlers(index)));
    transports.forEach((transport, index) => transport.peer.addEventListener("icecandidateerror", (event) => {
        diagnostics[index].iceErrors.push(event.errorCode);
    }));
    links = [new WasmSecureLink(module, options(0, selves[0], peers[0], attemptId), secureHandlers(0)),
             new WasmSecureLink(module, options(1, selves[1], peers[1], attemptId), secureHandlers(1))];
    outboxes = peers.map((peer) => new ProxyOutbox(proxy, peer.inbox, {
        onPublished: () => {}, onError: (error) => reject(error),
    }));
    const timeout = setTimeout(() => reject(new Error("proxy exchange timed out")), 15000);
    await transports[0].startOffer(); await complete; await verifyRoutes(turn); clearTimeout(timeout);
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    const states = await Promise.all(transports.map(async (transport) => {
        const report = await transport.peer.getStats().catch(() => null);
        const pairs = report ? [...report.values()].filter((entry) => entry.type === "candidate-pair")
            .map((entry) => ({ state: entry.state, nominated: entry.nominated })) : null;
        return { peer: transport.peer.connectionState, ice: transport.peer.iceConnectionState,
            gathering: transport.peer.iceGatheringState, pairs };
    }));
    document.body.dataset.result = "fail";
    document.body.textContent = `fail: ${error.message}; diagnostics: ${JSON.stringify({ diagnostics, states, ready })}`;
} finally {
    for (const mailbox of mailboxes) mailbox.close(); for (const outbox of outboxes) outbox.close();
    for (const watcher of watchers) watcher.close(); for (const lease of leases) lease.close();
    for (const transport of transports) transport.close(); for (const link of links) link.close();
}
