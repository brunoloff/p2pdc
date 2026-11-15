import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { WasmDiscovery } from "../src/browser/wasm_discovery.mjs";
import { WasmSecureLink } from "../src/browser/wasm_secure_link.mjs";
import { WasmIncoming } from "../src/browser/wasm_incoming.mjs";

const identity = (seed, group = 5) => ({ identitySeed: new Uint8Array(32).fill(seed),
    recipientSecret: new Uint8Array(32).fill(seed + 2), capability: new Uint8Array(32).fill(group) });
const check = (value, message) => { if (!value) throw new Error(message); };
const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
const attemptId = new Uint8Array(16).fill(7);
let trackers = [], link, incoming, now = 1000, event;
try {
    const module = await createModule();
    const handlers = { onPeer: (peer) => { event = peer; }, onError: (reason) => { throw new Error(reason); } };
    const sender = new WasmDiscovery(module, identity(1), handlers);
    const local = new WasmDiscovery(module, identity(2), handlers);
    const watch = new WasmDiscovery(module, identity(2), handlers);
    const wrong = new WasmDiscovery(module, identity(2, 9), handlers);
    const wrongIdentity = new WasmDiscovery(module, identity(3), handlers);
    const wrongBox = new WasmDiscovery(module, { ...identity(2), recipientSecret: new Uint8Array(32).fill(10) }, handlers);
    trackers = [sender, local, watch, wrong, wrongIdentity, wrongBox];
    const senderEndpoint = sender.self(), localEndpoint = local.self();
    check(local.publish(now), "local publication failed");
    const lease = { discovery: local, self: () => now < 1120 ? local.self() : null };
    const subscription = { discovery: watch, healthy: () => true, current: (id) => watch.current(id, now) };
    incoming = new WasmIncoming(lease, subscription, { now: () => now });
    let resolveWire;
    const signal = new Promise((resolve) => { resolveWire = resolve; });
    link = new WasmSecureLink(module, { ...identity(1), role: "offerer", timeoutMs: 10000,
        remoteSigningKey: localEndpoint.signingKey, remoteBoxKey: localEndpoint.recipientKey,
        localInstance: senderEndpoint.instance, remoteInstance: localEndpoint.instance, attemptId }, {
        sendFrame: () => "accepted", bufferedAmount: () => 0, onSignal: resolveWire,
        onDescription: () => {}, onCandidate: () => {}, onReady: () => {}, onMessage: () => {},
        onWritable: () => {}, onControl: () => {}, onFailure: (reason) => { throw new Error(reason); },
    });
    check(link.localDescription(Uint8Array.of(1, 2), now) === "accepted", "offer creation failed");
    const wire = await signal;
    const nextSignal = new Promise((resolve) => { link.handlers.onSignal = resolve; });
    const encoder = new TextEncoder();
    check(link.localCandidate(encoder.encode("candidate:test"), encoder.encode("0"), now) === "accepted",
        "candidate creation failed");
    const candidate = await nextSignal;
    check(incoming.receive(candidate).status === "nonOffer", "authenticated candidate rejected");
    const alteredCandidate = candidate.slice(); alteredCandidate[alteredCandidate.length - 1] ^= 1;
    check(incoming.receive(alteredCandidate).status === "ignored", "tampered candidate accepted");
    check(incoming.receive(wire).status === "unknownPeer", "unknown peer was not retryable");
    const advertisement = sender.publish(now);
    check(watch.receive(advertisement, now) === "changed", "advertisement rejected");
    await Promise.resolve();
    for (let index = 0; index < 40; ++index)
        check(incoming.receive(wire, true).status === "otherOffer", "offer inspection mutated replay state");
    const modified = wire.slice(); modified[modified.length - 1] ^= 1;
    check(incoming.receive(modified).status === "ignored", "tampered offer accepted");
    const accepted = incoming.receive(wire);
    check(accepted.status === "admitted" && accepted.peer.handle === event.handle &&
        equal(accepted.attemptId, attemptId) && equal(accepted.wire, wire), "valid offer rejected");
    check(incoming.receive(wire).status === "ignored", "replayed offer accepted");
    const repeated = new WasmIncoming(lease, subscription, { now: () => now });
    check(repeated.receive(wire).status === "ignored", "replay survived wrapper recreation"); repeated.close();
    for (const mismatch of [wrong, wrongIdentity, wrongBox]) {
        const other = new WasmIncoming(lease, { discovery: mismatch, healthy: () => true, current: () => null },
            { now: () => now });
        check(other.receive(wire).status === "failed", "mismatched context accepted"); other.close();
    }
    ++now;
    check(watch.receive(sender.publish(now), now) === "changed", "updated advertisement rejected");
    check(watch.current(accepted.peer.handle, now) === null, "updated peer handle remained current");
    now = 1120;
    check(incoming.receive(wire).status === "notReady", "expired local endpoint accepted");
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { incoming?.close(); link?.close(); for (const tracker of trackers) tracker.close(); }
