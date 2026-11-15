import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { WasmDiscovery } from "../src/browser/wasm_discovery.mjs";

const equal = (a, b) => a.length === b.length && a.every((value, index) => value === b[index]);
const requireValue = (condition, message) => { if (!condition) throw new Error(message); };
const options = (seed, cap = 5) => ({ identitySeed: new Uint8Array(32).fill(seed),
    recipientSecret: new Uint8Array(32).fill(seed + 2), capability: new Uint8Array(32).fill(cap) });
let trackers = [];
const events = [], errors = [];
try {
    const module = await createModule();
    const handlers = { onPeer: (peer, event) => events.push({ peer, event }), onError: (reason) => errors.push(reason) };
    const a = new WasmDiscovery(module, options(1), handlers);
    const b = new WasmDiscovery(module, options(2), handlers);
    const wrong = new WasmDiscovery(module, options(3, 9), handlers);
    trackers = [a, b, wrong];
    requireValue(equal(a.key(), b.key()) && !equal(a.key(), wrong.key()), "group key mismatch");
    const expectedWindowKey = Uint8Array.from("ff5a0f2b5a33d37eea6202e74b197ab665b93cd3".match(/../g),
        (byte) => parseInt(byte, 16));
    const laterWindowKey = Uint8Array.from("c98a6c07d497a1b4d307c92581174c23de21e141".match(/../g),
        (byte) => parseInt(byte, 16));
    requireValue(equal(a.windowKey(0), expectedWindowKey) && equal(a.windowKey(119), b.windowKey(0)) &&
        !equal(a.windowKey(120), b.windowKey(119)) && !equal(a.windowKey(0), wrong.windowKey(0)) &&
        equal(a.windowKey(123456789 * 120), laterWindowKey),
        "window key mismatch");
    const self = a.self();
    requireValue(self.instance.some(Boolean) && self.inbox.some(Boolean), "empty endpoint identifiers");
    const first = a.publish(1000);
    requireValue(first?.length === 242, "first publication failed");
    requireValue(wrong.receive(first, 1000) === "ignored", "wrong group accepted");
    const altered = first.slice(); altered[altered.length - 1] ^= 1;
    requireValue(b.receive(altered, 1000) === "ignored", "altered advertisement accepted");
    requireValue(b.receive(first, 1000) === "changed", "advertisement rejected");
    await Promise.resolve();
    requireValue(events.length === 1 && events[0].event === "seen", "missing peer event");
    const firstPeer = events[0].peer;
    requireValue(equal(firstPeer.signingKey, self.signingKey) && equal(firstPeer.recipientKey, self.recipientKey) &&
                 equal(firstPeer.instance, self.instance) && equal(firstPeer.inbox, self.inbox) &&
                 firstPeer.sequence === 0n && firstPeer.issuedAt === 1000n, "incorrect verified peer");
    requireValue(b.current(firstPeer.handle, 1000) !== null, "current peer missing");
    const second = a.publish(1001);
    requireValue(b.receive(second, 1001) === "changed", "updated advertisement rejected");
    await Promise.resolve();
    requireValue(events.length === 2 && events[1].event === "updated" && events[1].peer.sequence === 1n, "missing update");
    const secondPeer = events[1].peer;
    requireValue(firstPeer.handle !== secondPeer.handle && b.current(firstPeer.handle, 1001) === null &&
                 b.current(secondPeer.handle, 1001) !== null, "stale handle remained current");
    requireValue(b.receive(first, 1001) === "ignored", "stale replay accepted");
    requireValue(b.sweep(1151), "expiry sweep failed");
    await Promise.resolve();
    requireValue(events.length === 3 && events[2].event === "expired" && events[2].peer.handle === secondPeer.handle &&
                 b.current(secondPeer.handle, 1151) === null, "expiry not applied");
    requireValue(!b.sweep(1150), "clock regression accepted");
    await Promise.resolve();
    requireValue(errors.includes("discovery") && b.receive(first, 1151) === "closed", "clock failure not terminal");
    const rotatedEvents = [];
    const rotatedHandlers = { onPeer: (peer, event) => rotatedEvents.push({ peer, event }), onError: () => {} };
    const rotator = new WasmDiscovery(module, options(4), rotatedHandlers);
    const receiver = new WasmDiscovery(module, options(5), rotatedHandlers);
    const limited = new WasmDiscovery(module, options(6), rotatedHandlers);
    trackers.push(rotator, receiver, limited);
    let rotatedFirst;
    for (let i = 0; i < 32; ++i) {
        const wire = rotator.publishWindowed(1000);
        requireValue(wire?.length === 242 && limited.publishWindowed(1000)?.length === 242,
            "window publication limit reached early");
        if (i === 0) rotatedFirst = wire;
    }
    requireValue(!limited.publishWindowed(1000), "window publication limit not enforced");
    requireValue(receiver.receive(rotatedFirst, 1000) === "changed", "first window record rejected");
    const rotatedLast = rotator.publishWindowed(1080);
    requireValue(rotatedLast?.length === 242 && receiver.receive(rotatedLast, 1080) === "changed",
        "publication after window rotation failed");
    await Promise.resolve();
    requireValue(rotatedEvents.at(-1)?.peer.sequence === 32n, "window rotation reset sequence");
    requireValue(!rotator.publish(1080), "publication mode switch accepted");
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { for (const tracker of trackers) tracker.close(); }
