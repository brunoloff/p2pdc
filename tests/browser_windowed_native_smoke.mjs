import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserWatch } from "../src/browser/browser_watch.mjs";
import { BrowserLease } from "../src/browser/browser_lease.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";

const identity = { identitySeed: new Uint8Array(32).fill(2), recipientSecret: new Uint8Array(32).fill(4),
    capability: new Uint8Array(32).fill(5) };
const equal = (a, b) => a.length === b.length && a.every((value, index) => value === b[index]);
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let now = 1000, watch, lease, published = 0, nativeSequence = null, failure = null;
const fail = (error) => { failure ??= new Error(String(error)); };

async function waitFor(condition, timeout = 20000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (failure) throw failure;
        if (await condition()) return;
        await delay(50);
    }
    throw new Error("discovery exchange timed out");
}
async function nativeResult() {
    const response = await fetch("/interop-events", { cache: "no-store", signal: AbortSignal.timeout(5000) });
    if (!response.ok) throw new Error("native status unavailable");
    return response.json();
}
async function send(kind) {
    const response = await fetch("/interop-signal", { method: "POST", signal: AbortSignal.timeout(5000),
        headers: { "Content-Type": "application/json" }, body: JSON.stringify({ kind }) });
    if (!response.ok) throw new Error("native control failed");
}

try {
    const module = await createModule(), proxy = new OpenDhtProxy("http://127.0.0.1:49412", { timeoutMs: 5000 });
    const options = { windowed: true, now: () => now };
    lease = new BrowserLease(module, proxy, identity, { onPublished: () => ++published, onError: fail }, options);
    const local = lease.endpoint();
    watch = new BrowserWatch(module, proxy, identity, {
        onPeer: (peer, event) => {
            if (event !== "expired" && !equal(peer.signingKey, local.signingKey)) nativeSequence = peer.sequence;
        }, onCatchup: () => {}, onError: fail,
    }, options);
    if (!await watch.start() || !lease.start()) throw new Error("browser startup failed");
    await waitFor(() => published >= 1 && nativeSequence === 0n);
    await waitFor(async () => {
        const result = await nativeResult();
        if (result.result !== "pending") throw new Error(`native peer: ${result.result}`);
        return result.events.some((event) => event.kind === "wire" && event.data === "01");
    });
    await send("done");
    now = 1120; watch.tick();
    await waitFor(() => watch.healthy());
    lease.clearTimer(); lease.tick("refresh");
    await waitFor(() => published >= 2 && nativeSequence === 1n);
    await waitFor(async () => (await nativeResult()).result === "exchange");
    await send("stop");
    await waitFor(async () => (await nativeResult()).result === "pass");
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { lease?.close(); watch?.close(); }
