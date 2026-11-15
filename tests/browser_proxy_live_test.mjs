import test from "node:test";
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";

test("controlled proxy publishes, listens and retrieves opaque bytes", async () => {
    const url = process.env.P2PDC_TEST_PROXY_URL;
    if (!url) throw new Error("P2PDC_TEST_PROXY_URL is required");
    const proxy = new OpenDhtProxy(url, { timeoutMs: 8000 });
    const key = Uint8Array.from(randomBytes(20));
    const payload = Uint8Array.of(0, 255, 42, 128);
    let found;
    const delivered = new Promise((resolve) => { found = resolve; });
    const listener = proxy.listen(key, (value) => {
        if (!value.expired && value.bytes.length === payload.length &&
            value.bytes.every((byte, index) => byte === payload[index])) found(value);
    });
    let timeout;
    try {
        await new Promise((resolve) => setTimeout(resolve, 250));
        await proxy.put(key, payload);
        const result = await Promise.race([
            delivered,
            new Promise((_resolve, reject) => { timeout = setTimeout(() => reject(new Error("listen timed out")), 8000); }),
        ]);
        assert.deepEqual(result.bytes, payload);
        const values = await proxy.get(key);
        assert.ok(values.some((value) => value.bytes.length === payload.length &&
                  value.bytes.every((byte, index) => byte === payload[index])));
    } finally {
        clearTimeout(timeout);
        listener.close();
        await listener.done;
    }
});
