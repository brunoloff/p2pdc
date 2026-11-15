import test from "node:test";
import assert from "node:assert/strict";
import { WasmReliableLink } from "../src/browser/wasm_reliable_link.mjs";

function fixture() {
    const calls = [];
    const module = { HEAPU8: new Uint8Array(10000), _malloc: () => 16, _free: (p) => calls.push(["free", p]),
        _p2pdc_reliable_create: () => 1, _p2pdc_reliable_destroy: (id) => calls.push(["destroy", id]),
        _p2pdc_reliable_ready: (id, ready) => calls.push(["ready", id, ready]),
        _p2pdc_reliable_wake: (id) => calls.push(["wake", id]),
        _p2pdc_reliable_send: (id, ptr, size) => { calls.push(["send", id, [...module.HEAPU8.slice(ptr, ptr + size)]]); return 0; },
        _p2pdc_reliable_frame: (id, ptr, size) => calls.push(["frame", id, [...module.HEAPU8.slice(ptr, ptr + size)]]) };
    const messages = [], failures = [];
    const handlers = { sendFrame: () => "accepted", bufferedAmount: () => 0,
        onMessage: (value) => messages.push(value), onFailure: (value) => failures.push(value), onWritable: () => {} };
    return { module, calls, handlers, messages, failures };
}

test("bridge copies views, drives readiness and suppresses late delivery", async () => {
    const f = fixture();
    const link = new WasmReliableLink(f.module, f.handlers);
    assert.equal(link.setReady(true, 1024), true);
    const bytes = Uint8Array.of(9, 0, 255, 7).subarray(1, 3);
    assert.equal(link.send(bytes), "accepted");
    assert.deepEqual(f.calls.find((call) => call[0] === "send")[2], [0, 255]);
    assert.equal(link.onFrame(Uint8Array.of(3)), true);
    assert.deepEqual(f.calls.find((call) => call[0] === "frame")[2], [3]);
    f.module.p2pdcReliableBridge.message(link.id, Uint8Array.of(8));
    link.close(); await Promise.resolve();
    assert.equal(f.messages.length, 0);
    assert.equal(link.send(Uint8Array.of(1)), "closed");
});

test("invalid limits, callbacks and frames fail closed", async () => {
    const f = fixture();
    assert.throws(() => new WasmReliableLink(f.module, {}), TypeError);
    const link = new WasmReliableLink(f.module, f.handlers);
    assert.equal(link.setReady(true, 1023), false);
    assert.equal(link.send(Uint8Array.of(1)), "failed");
    await Promise.resolve();
    assert.deepEqual(f.failures, ["transportLimit"]);
    link.close();
    const other = fixture();
    const second = new WasmReliableLink(other.module, other.handlers);
    assert.equal(second.onFrame(new Uint8Array(1025)), false);
    await Promise.resolve();
    assert.deepEqual(other.failures, ["invalidFrame"]);
    second.close();
});

test("bridge bounds deferred callbacks and maps sink backpressure", async () => {
    const f = fixture();
    f.handlers.sendFrame = () => "backpressure";
    const link = new WasmReliableLink(f.module, f.handlers);
    assert.equal(f.module.p2pdcReliableBridge.send(link.id, Uint8Array.of(1)), 1);
    for (let i = 0; i < 128; ++i) assert.equal(f.module.p2pdcReliableBridge.message(link.id, Uint8Array.of(i)), 1);
    assert.equal(f.module.p2pdcReliableBridge.message(link.id, Uint8Array.of(1)), 0);
    await Promise.resolve();
    assert.equal(f.messages.length, 128);
    link.close();
});

test("copy ignores overridden typed array methods and allocation errors close the link", async () => {
    class TrickyBytes extends Uint8Array {
        get length() { return 0; }
        slice() { throw new Error("overridden slice"); }
    }
    const f = fixture();
    const link = new WasmReliableLink(f.module, f.handlers);
    assert.equal(link.send(new TrickyBytes([1, 2])), "accepted");
    assert.deepEqual(f.calls.find((call) => call[0] === "send")[2], [1, 2]);
    assert.equal(link.send(new TrickyBytes(262146)), "tooLarge");
    f.module._malloc = () => { throw new Error("out of memory"); };
    assert.equal(link.send(Uint8Array.of(1)), "failed");
    await Promise.resolve();
    assert.deepEqual(f.failures, ["allocation"]);
    link.close();
});

test("a synchronous backend failure is reflected by readiness and frame calls", async () => {
    const f = fixture();
    const link = new WasmReliableLink(f.module, f.handlers);
    f.module._p2pdc_reliable_ready = () => f.module.p2pdcReliableBridge.failure(link.id, 3);
    assert.equal(link.setReady(true, 4096), false);
    await Promise.resolve();
    assert.deepEqual(f.failures, ["allocation"]);
    link.close();
    const g = fixture();
    const second = new WasmReliableLink(g.module, g.handlers);
    g.module._p2pdc_reliable_frame = () => g.module.p2pdcReliableBridge.failure(second.id, 1);
    assert.equal(second.onFrame(Uint8Array.of(0)), false);
    await Promise.resolve();
    assert.deepEqual(g.failures, ["invalidFrame"]);
    second.close();
});
