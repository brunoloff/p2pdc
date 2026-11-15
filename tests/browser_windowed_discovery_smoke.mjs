import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserLease } from "../src/browser/browser_lease.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";

const identity = (seed) => ({ identitySeed: new Uint8Array(32).fill(seed),
    recipientSecret: new Uint8Array(32).fill(seed + 2), capability: new Uint8Array(32).fill(5) });
const keyId = (key) => Array.from(key).join(",");
const requireValue = (value, message) => { if (!value) throw new Error(message); };
const settle = async () => { for (let i = 0; i < 40; ++i) await Promise.resolve(); };

function localProxy() {
    const records = new Map(), listeners = new Map();
    return {
        listen(key, callback) {
            const id = keyId(key), list = listeners.get(id) ?? new Set();
            let finish; const done = new Promise((resolve) => { finish = resolve; });
            listeners.set(id, list); list.add(callback);
            return { ready: Promise.resolve(true), done, close: () => { list.delete(callback); finish(); } };
        },
        async get(key) { return records.get(keyId(key)) ?? []; },
        async put(key, wire) {
            const id = keyId(key), record = { bytes: wire.slice(), expired: false };
            const list = records.get(id) ?? []; list.push(record); records.set(id, list);
            for (const callback of listeners.get(id) ?? []) callback(record);
        },
    };
}

function manualClock() {
    let now = 1000, next = 0;
    const pending = new Map();
    return { options: { now: () => now, windowed: true, timers: {
        set: (callback, delay) => { const id = ++next; pending.set(id, { callback, delay }); return id; },
        clear: (id) => pending.delete(id),
    } }, setNow: (value) => { now = value; }, fire: (delay) => {
        const entry = [...pending].find(([, timer]) => timer.delay === delay);
        if (!entry) throw new Error("expected timer missing");
        pending.delete(entry[0]); entry[1].callback();
    } };
}

let lease, watch;
try {
    const module = await createModule(), proxy = localProxy(), clock = manualClock();
    const events = [], errors = [], statuses = [];
    watch = new BrowserWatch(module, proxy, identity(1), {
        onPeer: (peer, event) => events.push({ peer, event }), onCatchup: () => {},
        onError: (error) => errors.push(error), onStatus: (status) => statuses.push(status),
    }, clock.options);
    lease = new BrowserLease(module, proxy, identity(2), {
        onPublished: () => {}, onError: (error) => errors.push(error),
    }, clock.options);
    requireValue(await watch.start(), "watch failed to start");
    requireValue(lease.start(), "lease failed to start"); await settle();
    requireValue(events.length === 1 && events[0].event === "seen" && events[0].peer.sequence === 0n,
        "initial peer missing");
    clock.setNow(1120); watch.tick(); await settle();
    clock.fire(60000); await settle();
    requireValue(events.length === 2 && events[1].event === "updated" && events[1].peer.sequence === 1n,
        "peer did not update across windows");
    requireValue(statuses.includes("reconnecting") && statuses.includes("ready") && errors.length === 0,
        "window transition status failed");
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { lease?.close(); watch?.close(); }
