import test from "node:test";
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";
import { RecoveringMailbox } from "../src/browser/recovering_mailbox.mjs";

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}

test("controlled proxy catches up after an interrupted discovery stream", async () => {
    const url = process.env.P2PDC_TEST_PROXY_URL;
    if (!url) throw new Error("P2PDC_TEST_PROXY_URL is required");
    const backend = new OpenDhtProxy(url, { timeoutMs: 8000 });
    const broken = deferred(), disconnected = deferred(), received = deferred();
    let first = true;
    const proxy = {
        listen(key, handler, signal) {
            const subscription = backend.listen(key, handler, signal);
            if (!first) return subscription;
            first = false;
            return { ready: subscription.ready, done: Promise.race([subscription.done, broken.promise]),
                close: () => subscription.close() };
        },
        get: (...args) => backend.get(...args),
    };
    const timers = { set: (fn, delay) => setTimeout(fn, delay), clear: (id) => clearTimeout(id) };
    const key = Uint8Array.from(randomBytes(20)), payload = Uint8Array.of(91, 0, 255);
    const mailbox = new RecoveringMailbox(proxy, key, {
        onRecord: (bytes) => { if (bytes.length === 3 && bytes.every((v, i) => v === payload[i])) received.resolve(bytes); },
        onCatchup: () => {}, onStatus: (state) => { if (state === "reconnecting") disconnected.resolve(); },
        onError: (error) => received.reject(error),
    }, timers, { retryDelayMs: 1000 });
    let deadline;
    try {
        assert.equal(await mailbox.start(), true);
        broken.reject(new ProxyError("stream_closed", "test interruption"));
        await disconnected.promise;
        await backend.put(key, payload);
        const result = await Promise.race([received.promise, new Promise((_resolve, reject) => {
            deadline = setTimeout(() => reject(new Error("recovery timed out")), 12000);
        })]);
        assert.deepEqual(result, payload);
        assert.equal(mailbox.healthy(), true);
    } finally { clearTimeout(deadline); mailbox.close(); }
});
