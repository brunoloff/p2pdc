import test from "node:test";
import assert from "node:assert/strict";
import { WasmDiscovery } from "../src/browser/wasm_discovery.mjs";

function fixture() {
    const frees = [], events = [], errors = [];
    const module = { HEAPU8: new Uint8Array(4096), _malloc: () => 100,
        _free: (pointer) => frees.push([...module.HEAPU8.slice(pointer, pointer + 96)]),
        _p2pdc_discovery_create: () => 1, _p2pdc_discovery_destroy: () => {},
        _p2pdc_discovery_healthy: () => 1,
        _p2pdc_discovery_key: (_id, pointer, size) => { module.HEAPU8.fill(7, pointer, pointer + size); return 1; },
        _p2pdc_discovery_window_key: (_id, now, pointer, size) => { module.HEAPU8.fill(now % 256, pointer, pointer + size); return 1; },
        _p2pdc_discovery_self: (_id, pointer, size) => { module.HEAPU8.fill(8, pointer, pointer + size); return 1; },
        _p2pdc_discovery_publish: (_id, _now, pointer, size) => { module.HEAPU8.fill(9, pointer, pointer + size); return 1; },
        _p2pdc_discovery_publish_windowed: (_id, _now, pointer, size) => { module.HEAPU8.fill(10, pointer, pointer + size); return 1; },
        _p2pdc_discovery_receive: () => 1, _p2pdc_discovery_sweep: () => 1,
        _p2pdc_discovery_current: () => 0 };
    const options = { identitySeed: new Uint8Array(32).fill(1), recipientSecret: new Uint8Array(32).fill(2),
        capability: new Uint8Array(32).fill(3) };
    const handlers = { onPeer: (peer, event) => events.push({ peer, event }), onError: (error) => errors.push(error) };
    return { module, options, handlers, frees, events, errors };
}
async function settle() { for (let i = 0; i < 6; ++i) await Promise.resolve(); }

test("discovery wipes temporary credentials and returns copied outputs", () => {
    const f = fixture();
    const discovery = new WasmDiscovery(f.module, f.options, f.handlers);
    assert.ok(f.frees[0].every((value) => value === 0));
    assert.equal(f.options.identitySeed[0], 1);
    assert.deepEqual([...discovery.key()], Array(20).fill(7));
    assert.equal(discovery.self().inbox.length, 20);
    assert.equal(discovery.publish(100).length, 242);
    assert.equal(discovery.receive(Uint8Array.of(1), 100), "ignored");
    discovery.close(); assert.equal(discovery.key(), null);
});

test("window key checks time and reports a missing bridge export", () => {
    const f = fixture(), discovery = new WasmDiscovery(f.module, f.options, f.handlers);
    assert.deepEqual([...discovery.windowKey(120)], Array(20).fill(120));
    assert.throws(() => discovery.windowKey(-1), RangeError);
    assert.throws(() => discovery.windowKey(Number.MAX_SAFE_INTEGER), RangeError);
    delete f.module._p2pdc_discovery_window_key;
    assert.throws(() => discovery.windowKey(0), TypeError);
    discovery.close();
});

test("windowed publication uses its own bridge and reports a missing export", () => {
    const f = fixture(), discovery = new WasmDiscovery(f.module, f.options, f.handlers);
    assert.deepEqual([...discovery.publishWindowed(120)], Array(242).fill(10));
    delete f.module._p2pdc_discovery_publish_windowed;
    assert.throws(() => discovery.publishWindowed(120), TypeError);
    discovery.close();
});

test("discovery events preserve large counters and suppress delivery on close", async () => {
    const f = fixture();
    const discovery = new WasmDiscovery(f.module, f.options, f.handlers);
    const value = new Uint8Array(132);
    new DataView(value.buffer).setBigUint64(100, 9007199254740993n);
    assert.equal(f.module.p2pdcDiscoveryBridge.event(1, 9, 1, value), true);
    await settle();
    assert.equal(f.events[0].event, "updated");
    assert.equal(f.events[0].peer.sequence, 9007199254740993n);
    assert.equal(f.events[0].peer.handle, 9);
    assert.equal(f.module.p2pdcDiscoveryBridge.event(1, 10, 0, value), true);
    discovery.close(); await settle();
    assert.equal(f.events.length, 1);
});

test("discovery rejects bad input, bounds pending events and reports terminal errors", async () => {
    const f = fixture();
    assert.throws(() => new WasmDiscovery(f.module, { ...f.options, capability: new Uint8Array(31) }, f.handlers), TypeError);
    const discovery = new WasmDiscovery(f.module, f.options, f.handlers);
    const value = new Uint8Array(132);
    for (let i = 0; i < 128; ++i) assert.equal(f.module.p2pdcDiscoveryBridge.event(1, i + 1, 0, value), true);
    assert.equal(f.module.p2pdcDiscoveryBridge.event(1, 129, 0, value), false);
    f.module._p2pdc_discovery_sweep = () => 0;
    assert.equal(discovery.sweep(100), false);
    await settle();
    assert.deepEqual(f.errors, ["discovery"]);
    assert.equal(f.events.length, 0);
    assert.equal(discovery.receive(new Uint8Array(242), 100), "closed");
    discovery.close();
});

test("malformed records still perform the clock and expiry sweep", async () => {
    const f = fixture();
    const discovery = new WasmDiscovery(f.module, f.options, f.handlers);
    let clock;
    f.module._p2pdc_discovery_sweep = (_id, value) => { clock = value; return 0; };
    assert.equal(discovery.receive(Uint8Array.of(1), 99), "failed");
    assert.equal(clock, 99);
    await settle();
    assert.deepEqual(f.errors, ["discovery"]);
    discovery.close();
});
