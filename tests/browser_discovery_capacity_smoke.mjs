import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { WasmDiscovery } from "../src/browser/wasm_discovery.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";
import { BrowserNode } from "../src/browser/browser_node.mjs";

const identity = (seed) => ({ identitySeed: new Uint8Array(32).fill(seed),
    recipientSecret: new Uint8Array(32).fill(seed + 100), capability: new Uint8Array(32).fill(5) });
const requireValue = (value, message) => { if (!value) throw new Error(message); };
let watch, node;
try {
    const module = await createModule(), records = [], handlers = { onPeer: () => {}, onError: () => {} };
    let key;
    for (let seed = 1; seed <= 100; ++seed) {
        const peer = new WasmDiscovery(module, identity(seed), handlers);
        key ??= peer.windowKey(1000);
        for (const time of [1000, 1001]) {
            const wire = peer.publishWindowed(time);
            requireValue(wire?.length === 242, "announcement generation failed");
            records.push({ bytes: wire, expired: false });
        }
        peer.close();
    }
    const proxy = {
        listen() { let close; const done = new Promise((resolve) => { close = resolve; });
            return { ready: Promise.resolve(true), done, close }; },
        async get(requested) {
            return requested.every((value, index) => value === key[index]) ? records : [];
        },
    };
    const events = [], errors = [];
    watch = new BrowserWatch(module, proxy, identity(201), {
        onPeer: (peer, event) => events.push({ peer, event }), onCatchup: () => {}, onError: (error) => errors.push(error),
    }, { windowed: true, now: () => 1001 });
    requireValue(await watch.start(), "watch failed to start");
    for (let count = 0; count < 500; ++count) await Promise.resolve();
    requireValue(errors.length === 0 && watch.healthy(), `watch failed: ${errors.join(",")}`);
    requireValue(events.filter(({ event }) => event === "seen").length === 100 &&
        events.filter(({ event }) => event === "updated").length === 100, "missing peer events");
    watch.close();
    const found = new Set(), nodeErrors = [];
    node = new BrowserNode(module, proxy, identity(202), {
        onPeer: (peer) => found.add(Array.from(peer.signingKey).join(",")), onCatchup: () => {},
        onError: (error) => nodeErrors.push(error),
    }, { watch: { windowed: true, now: () => 1001 } });
    requireValue(await node.start(), "node failed to start");
    for (let count = 0; count < 500; ++count) await Promise.resolve();
    requireValue(nodeErrors.length === 0 && found.size === 100, `node missed peers: ${found.size}`);
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { node?.close(); watch?.close(); }
