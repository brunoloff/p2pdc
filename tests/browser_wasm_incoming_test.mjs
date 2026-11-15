import test from "node:test";
import assert from "node:assert/strict";
import { WasmIncoming } from "../src/browser/wasm_incoming.mjs";

function fixture() {
    let result = 1, published = true, healthy = true, now = 1000, frees = 0;
    const peer = { handle: 7 };
    const module = { HEAPU8: new Uint8Array(65536), _malloc: () => 128, _free: () => ++frees,
        _p2pdc_discovery_incoming: (_a, _b, _wire, _size, _now, _inspect, out) => {
            module.HEAPU8.fill(0, out, out + 20); module.HEAPU8[out + 3] = 7;
            module.HEAPU8.fill(9, out + 4, out + 20); return result;
        } };
    const lease = { discovery: { module, id: 1 }, self: () => published ? {} : null };
    const watch = { discovery: { module, id: 2 }, healthy: () => healthy, current: (handle) => handle === 7 ? peer : null };
    return { module, lease, watch, peer, options: { now: () => now }, setResult: (v) => { result = v; },
        setPublished: (v) => { published = v; }, setHealthy: (v) => { healthy = v; },
        setNow: (v) => { now = v; }, frees: () => frees };
}

test("incoming admission returns independent bytes and current verified peer", () => {
    const f = fixture(), incoming = new WasmIncoming(f.lease, f.watch, f.options);
    const wire = Uint8Array.of(1, 2), result = incoming.receive(wire);
    assert.equal(result.status, "admitted"); assert.equal(result.peer, f.peer);
    wire[0] = 9; assert.deepEqual([...result.wire], [1, 2]);
    assert.deepEqual([...result.attemptId], Array(16).fill(9)); assert.equal(f.frees(), 1);
    incoming.close(); assert.deepEqual(incoming.receive(wire), { status: "closed" });
});

test("incoming admission distinguishes waiting, ignored, unknown and terminal errors", () => {
    const f = fixture(), incoming = new WasmIncoming(f.lease, f.watch, f.options);
    f.setPublished(false); assert.equal(incoming.receive(Uint8Array.of(1)).status, "notReady");
    f.setPublished(true); f.setHealthy(false); assert.equal(incoming.receive(Uint8Array.of(1)).status, "notReady");
    f.setHealthy(true); f.setResult(0); assert.equal(incoming.receive(Uint8Array.of(1)).status, "ignored");
    f.setResult(3); assert.equal(incoming.receive(Uint8Array.of(1)).status, "unknownPeer");
    f.setResult(4); assert.equal(incoming.receive(Uint8Array.of(1)).status, "nonOffer");
    f.setResult(5); assert.equal(incoming.receive(Uint8Array.of(1), true).status, "otherOffer");
    assert.equal(incoming.receive(new Uint8Array(32769)).status, "ignored");
    f.setResult(2); assert.equal(incoming.receive(Uint8Array.of(1)).status, "failed");
    f.setResult(1); assert.equal(incoming.receive(Uint8Array.of(1)).status, "failed");
    incoming.close();
});

test("incoming admission rejects bad inputs and regressing time", () => {
    const f = fixture();
    assert.throws(() => new WasmIncoming(f.lease, { ...f.watch, discovery: { module: {} } }), TypeError);
    const incoming = new WasmIncoming(f.lease, f.watch, f.options);
    assert.throws(() => incoming.receive("record"), TypeError);
    assert.throws(() => incoming.receive(Uint8Array.of(1), "yes"), TypeError);
    assert.equal(incoming.receive(Uint8Array.of(1)).status, "admitted");
    f.setNow(999); assert.equal(incoming.receive(Uint8Array.of(1)).status, "failed");
    incoming.close();
});

test("incoming admission pins its discovery owners", () => {
    const f = fixture(), incoming = new WasmIncoming(f.lease, f.watch, f.options);
    f.watch.discovery = { ...f.watch.discovery };
    assert.equal(incoming.receive(Uint8Array.of(1)).status, "failed");
    incoming.close();
    const g = fixture(), other = new WasmIncoming(g.lease, g.watch, g.options);
    g.lease.discovery.id = 4;
    assert.equal(other.receive(Uint8Array.of(1)).status, "failed");
    other.close();
    const h = fixture(), third = new WasmIncoming(h.lease, h.watch, h.options);
    h.watch.closed = true;
    assert.equal(third.receive(Uint8Array.of(1)).status, "failed");
    third.close();
});
