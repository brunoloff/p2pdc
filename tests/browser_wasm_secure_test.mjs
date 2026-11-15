import test from "node:test";
import assert from "node:assert/strict";
import { WasmSecureLink } from "../src/browser/wasm_secure_link.mjs";

function fixture() {
    const calls = [];
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 128,
        _free: (p) => calls.push(["free", p, [...module.HEAPU8.slice(p, p + 213)]]),
        _p2pdc_secure_create: () => 1, _p2pdc_secure_destroy: (id) => calls.push(["destroy", id]),
        _p2pdc_secure_description: () => 0, _p2pdc_secure_candidate: () => 0,
        _p2pdc_secure_receive: () => 0, _p2pdc_secure_description_done: (id, ok) => { calls.push(["descriptionDone", id, ok]); return 0; },
        _p2pdc_secure_candidate_done: (id, op, ok) => { calls.push(["candidateDone", id, op, ok]); return 0; },
        _p2pdc_secure_ready: () => 0, _p2pdc_secure_frame: () => 0,
        _p2pdc_secure_send: () => 0, _p2pdc_secure_wake: () => 0, _p2pdc_secure_timeout: () => 0 };
    const options = { role: "offerer", timeoutMs: 10000, identitySeed: new Uint8Array(32).fill(1),
        recipientSecret: new Uint8Array(32).fill(2), capability: new Uint8Array(32).fill(3),
        remoteSigningKey: new Uint8Array(32).fill(4), remoteBoxKey: new Uint8Array(32).fill(5),
        localInstance: new Uint8Array(16).fill(6), remoteInstance: new Uint8Array(16).fill(7), attemptId: new Uint8Array(16).fill(8) };
    const seen = { messages: [], failures: [], ready: [] };
    const handlers = { sendFrame: () => "accepted", bufferedAmount: () => 0, onSignal: () => {},
        onDescription: () => {}, onCandidate: () => {}, onReady: (value) => seen.ready.push(value),
        onMessage: (value) => seen.messages.push(value), onWritable: () => {},
        onFailure: (value) => seen.failures.push(value), onControl: () => {} };
    return { module, options, calls, handlers, seen };
}
async function settle() { for (let index = 0; index < 8; ++index) await Promise.resolve(); }

test("creation wipes temporary credentials while retaining caller ownership", () => {
    const f = fixture();
    const link = new WasmSecureLink(f.module, f.options, f.handlers);
    assert.ok(f.calls[0][2].every((value) => value === 0));
    assert.equal(f.options.identitySeed[0], 1);
    assert.equal(link.setTransportReady(true, 1024), true);
    assert.equal(link.send(Uint8Array.of(0, 255)), "accepted");
    assert.equal(link.send(new Uint8Array(262145)), "tooLarge");
    link.close();
    assert.equal(link.send(Uint8Array.of(1)), "closed");
});

test("message limit is encoded and incompatible WebAssembly builds fail", () => {
    const f = fixture(); let captured;
    f.module._p2pdc_secure_create = (ptr, size) => {
        captured = f.module.HEAPU8.slice(ptr, ptr + size); return 1;
    };
    const link = new WasmSecureLink(f.module, { ...f.options, maxMessageBytes: 8192 }, f.handlers);
    assert.equal(captured.length, 213); assert.equal(new DataView(captured.buffer).getUint32(209), 8192);
    link.close();
    for (const limit of [0, -1, 4194305, 1.5])
        assert.throws(() => new WasmSecureLink(f.module, { ...f.options, maxMessageBytes: limit }, f.handlers), RangeError);
    const stale = fixture(); stale.module._p2pdc_secure_create = (_ptr, size) => size === 209 ? 1 : 0;
    assert.throws(() => new WasmSecureLink(stale.module, stale.options, stale.handlers), /cannot create/);
});

test("large messages respect the configured send and delivery bounds", async () => {
    const f = fixture();
    const limit = 4 * 1024 * 1024;
    f.module.HEAPU8 = new Uint8Array(limit + 256);
    f.handlers.onMessage = () => new Promise(() => {});
    const link = new WasmSecureLink(f.module, { ...f.options, maxMessageBytes: limit }, f.handlers);
    assert.equal(link.send(new Uint8Array(limit)), "accepted");
    assert.equal(link.send(new Uint8Array(limit + 1)), "tooLarge");
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 2, new Uint8Array(limit)), true);
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 2, Uint8Array.of(1)), false);
    link.close(); await settle();
});

test("description and candidate completions wait for asynchronous handlers", async () => {
    const f = fixture();
    let finishDescription, finishCandidate;
    f.handlers.onDescription = () => new Promise((resolve) => { finishDescription = resolve; });
    f.handlers.onCandidate = () => new Promise((resolve) => { finishCandidate = resolve; });
    const link = new WasmSecureLink(f.module, f.options, f.handlers);
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 1, Uint8Array.of(1)), true);
    assert.equal(f.module.p2pdcSecureBridge.candidate(1, 7, Uint8Array.of(2), Uint8Array.of(3)), true);
    await settle();
    assert.equal(f.calls.some((call) => call[0].endsWith("Done")), false);
    finishDescription(); finishCandidate(); await settle();
    assert.ok(f.calls.some((call) => call[0] === "descriptionDone" && call[2] === 1));
    assert.ok(f.calls.some((call) => call[0] === "candidateDone" && call[2] === 7 && call[3] === 1));
    link.close();
});

test("close suppresses late completions and failed readiness is visible", async () => {
    const f = fixture();
    let finish;
    f.handlers.onDescription = () => new Promise((resolve) => { finish = resolve; });
    const link = new WasmSecureLink(f.module, f.options, f.handlers);
    f.module.p2pdcSecureBridge.bytes(1, 1, Uint8Array.of(1)); await settle();
    link.close(); finish(); await settle();
    assert.equal(f.calls.some((call) => call[0] === "descriptionDone"), false);
    const g = fixture();
    const other = new WasmSecureLink(g.module, g.options, g.handlers);
    assert.equal(other.setTransportReady(true, 1023), false);
    await settle();
    assert.deepEqual(g.seen.failures, ["transportLimit"]);
    other.close();
});

test("notification queues are bounded and reject unsupported configuration", async () => {
    const f = fixture();
    assert.throws(() => new WasmSecureLink(f.module, { ...f.options, identitySeed: new Uint8Array(31) }, f.handlers), TypeError);
    f.handlers.onMessage = () => new Promise(() => {});
    const link = new WasmSecureLink(f.module, f.options, f.handlers);
    for (let index = 0; index < 128; ++index) assert.equal(f.module.p2pdcSecureBridge.bytes(1, 2, Uint8Array.of(1)), true);
    assert.equal(f.module.p2pdcSecureBridge.bytes(1, 2, Uint8Array.of(1)), false);
    link.close(); await settle();
});

test("readiness event queues and rejected description reports failure", async () => {
    const f = fixture();
    f.handlers.onDescription = async () => { throw new Error("invalid SDP"); };
    const link = new WasmSecureLink(f.module, f.options, f.handlers);
    assert.equal(f.module.p2pdcSecureBridge.number(1, 0, 100), true);
    f.module.p2pdcSecureBridge.bytes(1, 1, Uint8Array.of(1));
    await settle();
    assert.deepEqual(f.seen.ready, [100]);
    assert.ok(f.calls.some((call) => call[0] === "descriptionDone" && call[2] === 0));
    link.close();
});

test("terminal control is delivered before failure unless explicitly closed", async () => {
    const f = fixture(), order = [];
    f.handlers.onControl = () => order.push("control");
    f.handlers.onFailure = () => order.push("failure");
    const link = new WasmSecureLink(f.module, f.options, f.handlers);
    assert.equal(f.module.p2pdcSecureBridge.number(1, 3, 4), true);
    assert.equal(f.module.p2pdcSecureBridge.number(1, 2, 0), true);
    await settle();
    assert.deepEqual(order, ["control", "failure"]);
    link.close();
    const g = fixture(), later = [];
    g.handlers.onControl = () => later.push("control");
    const other = new WasmSecureLink(g.module, g.options, g.handlers);
    g.module.p2pdcSecureBridge.number(1, 3, 4);
    other.close(); await settle();
    assert.deepEqual(later, []);
});
