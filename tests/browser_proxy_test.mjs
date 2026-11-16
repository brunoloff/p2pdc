import test from "node:test";
import assert from "node:assert/strict";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { ProxyError, decodeRecord, keyHex, sameBytes } from "../src/browser/proxy_codec.mjs";

const key = Uint8Array.from({ length: 20 }, (_, index) => index + 1);
const encoder = new TextEncoder();
const record = (data, id = "1", extra = {}) => JSON.stringify({ data, id, type: 0, ...extra });
function response(chunks, status = 200) {
    return { ok: status >= 200 && status < 300, status, body: new ReadableStream({
        start(controller) {
            for (const chunk of chunks) controller.enqueue(typeof chunk === "string" ? encoder.encode(chunk) : chunk);
            controller.close();
        },
    }) };
}
function proxy(fetch, options = {}) { return new OpenDhtProxy("https://proxy.example/base", { ...options, fetch }); }

test("formats keys and rejects invalid endpoints and keys", () => {
    assert.equal(keyHex(key), "0102030405060708090a0b0c0d0e0f1011121314");
    assert.throws(() => keyHex(new Uint8Array(20)), TypeError);
    assert.throws(() => keyHex(new Uint8Array(19)), TypeError);
    assert.throws(() => new OpenDhtProxy("http://example.com"), TypeError);
    assert.throws(() => new OpenDhtProxy("https://u:p@example.com"), TypeError);
    assert.throws(() => new OpenDhtProxy("https://example.com/?token=x"), TypeError);
    assert.doesNotThrow(() => new OpenDhtProxy("http://127.0.0.1:8000"));
});

test("binary equality requires identical length and content", () => {
    assert.equal(sameBytes(Uint8Array.of(0, 255), Uint8Array.of(0, 255)), true);
    assert.equal(sameBytes(Uint8Array.of(0), Uint8Array.of(0, 1)), false);
    assert.equal(sameBytes(Uint8Array.of(0, 1), Uint8Array.of(0, 2)), false);
});

test("encodes binary put to the configured proxy", async () => {
    let request;
    const client = proxy(async (url, init) => { request = { url, init }; return response([]); });
    await client.put(key, Uint8Array.of(0, 255, 42));
    assert.equal(request.url, `https://proxy.example/base/key/${keyHex(key)}`);
    assert.equal(request.init.method, "POST");
    assert.equal(request.init.headers["Content-Type"], "text/plain;charset=UTF-8");
    assert.equal(request.init.credentials, "omit");
    assert.equal(request.init.redirect, "error");
    assert.deepEqual(JSON.parse(request.init.body), { data: "AP8q" });
    await assert.rejects(client.put(key, new Uint8Array(32769)), TypeError);
});

test("default browser fetch retains its global receiver", async () => {
    const previous = globalThis.fetch;
    globalThis.fetch = function () {
        assert.equal(this, globalThis);
        return Promise.resolve(response([]));
    };
    try {
        const client = new OpenDhtProxy("https://proxy.example");
        assert.deepEqual(await client.get(key), []);
    } finally { globalThis.fetch = previous; }
});

test("get parses split UTF-8 lines and deduplicates only identical ID and payload", async () => {
    const lines = `${record("AP8=", "1", { note: "é" })}\r\n${record("AP8=", "1")}\n${record("AQ==", "1")}\n${record("Ag==", "0")}`;
    const bytes = encoder.encode(lines);
    const chunks = Array.from(bytes, (byte) => Uint8Array.of(byte));
    const client = proxy(async () => response(chunks));
    const values = await client.get(key);
    assert.deepEqual(values.map((value) => [...value.bytes]), [[0, 255], [1], [2]]);
    assert.deepEqual(values.map((value) => value.id), ["1", "1", "0"]);
});

test("discovery lookup collapses identical bytes across IDs without hiding expiry", async () => {
    const lines = [record("AQ==", "1"), record("AQ==", "2"), record("AQ==", "3", { expired: true }),
        record("Ag==", "1")].join("\n");
    const client = proxy(async () => response([lines]), { maxValues: 3 });
    const values = await client.get(key, undefined, { deduplicateByContent: true });
    assert.deepEqual(values.map((value) => [[...value.bytes], value.expired]),
        [[[1], false], [[1], true], [[2], false]]);
    await assert.rejects(client.get(key, undefined, { deduplicateByContent: "yes" }), TypeError);
});

test("discovery lookup bounds inspection even for repeated content", async () => {
    const lines = [...Array.from({ length: 9 }, (_, index) => record("AQ==", String(index + 1))),
                   record("Ag==", "10")].join("\n");
    const client = proxy(async () => response([lines]), { maxValues: 1 });
    const values = await client.get(key, undefined, { deduplicateByContent: true });
    assert.deepEqual(values.map((value) => [...value.bytes]), [[1]]);
});

test("default lookup accepts 512 distinct records and drops the next", async () => {
    const lines = Array.from({ length: 513 }, (_, index) =>
        record(btoa(String.fromCharCode(index >> 8, index & 255)), String(index + 1)));
    const allowed = proxy(async () => response([lines.slice(0, 512).join("\n")]));
    assert.equal((await allowed.get(key)).length, 512);
    const exceeded = proxy(async () => response([lines.join("\n")]));
    assert.equal((await exceeded.get(key)).length, 512);
});

test("rejects malformed, noncanonical and oversized records", async () => {
    assert.throws(() => decodeRecord("{", 4), ProxyError);
    assert.throws(() => decodeRecord(record("A===", "1"), 4), ProxyError);
    assert.throws(() => decodeRecord(record("AB==", "1"), 4), ProxyError);
    assert.throws(() => decodeRecord(record("AA==", 4), 4), ProxyError);
    assert.throws(() => decodeRecord(record("AQI=", "1"), 1), ProxyError);
    const tooLong = proxy(async () => response([`${"x".repeat(5000)}\n${record("AQ==")}\n`]), { maxValueBytes: 1 });
    assert.deepEqual((await tooLong.get(key)).map((value) => [...value.bytes]), [[1]]);
    const split = proxy(async () => response(["x".repeat(3000), "x".repeat(3000), `\n${record("Ag==")}`]),
                        { maxValueBytes: 1 });
    assert.deepEqual((await split.get(key)).map((value) => [...value.bytes]), [[2]]);
    const oversized = proxy(async () => response([`${record("AQI=", "1")}\n${record("Aw==", "2")}\n`]),
                            { maxValueBytes: 1 });
    assert.deepEqual((await oversized.get(key)).map((value) => [...value.bytes]), [[3]]);
    const badUtf8 = proxy(async () => response([Uint8Array.of(0xff, 10)]));
    await assert.rejects(badUtf8.get(key), (error) => error.code === "invalid_record");
});

test("lookup enforces record and total byte budgets by dropping surplus records", async () => {
    const records = `${record("AQ==", "1")}\n${record("Ag==", "2")}\n`;
    const count = proxy(async () => response([records]), { maxValues: 1 });
    assert.deepEqual((await count.get(key)).map((value) => [...value.bytes]), [[1]]);
    const total = proxy(async () => response([records]), { maxTotalBytes: 1 });
    assert.deepEqual((await total.get(key)).map((value) => [...value.bytes]), [[1]]);
});

test("listen drops a record larger than its byte budget and keeps streaming", async () => {
    const values = [];
    const client = proxy(async () => response([`${record("AQI=")}\n${"x".repeat(50000)}\n${record("Aw==")}\n`]),
                         { maxTotalBytes: 1 });
    await assert.rejects(client.listen(key, (value) => values.push([...value.bytes])).done,
                         (error) => error.code === "stream_closed");
    assert.deepEqual(values, [[3]]);
});

test("listen preserves expiration and stops deliveries after close", async () => {
    const values = [];
    const client = proxy(async () => response([`${record("AQ==", "1", { expired: true })}\n${"x".repeat(50000)}\n`]));
    let subscription;
    subscription = client.listen(key, (value) => { values.push(value); subscription.close(); });
    await subscription.done;
    assert.equal(values.length, 1);
    assert.equal(values[0].expired, true);
    assert.deepEqual([...values[0].bytes], [1]);
});

test("unexpected listen EOF and HTTP errors are visible", async () => {
    const client = proxy(async () => response([]));
    await assert.rejects(client.listen(key, () => {}).done, (error) => error.code === "stream_closed");
    const noBody = proxy(async () => ({ ok: true, status: 200, body: null }));
    assert.deepEqual(await noBody.get(key), []);
    const unavailable = proxy(async () => response([], 503));
    await assert.rejects(unavailable.get(key), (error) => error.code === "http_error" && error.status === 503);
    await assert.rejects(unavailable.put(key, Uint8Array.of(1)), (error) => error.code === "http_error");
});

test("network request and stream read failures have recoverable error codes", async () => {
    const offline = proxy(async () => { throw new TypeError("network unavailable"); });
    await assert.rejects(offline.get(key), (error) => error.code === "network_error");
    const disconnected = proxy(async () => ({ ok: true, status: 200, body: new ReadableStream({
        start(controller) { controller.error(new TypeError("connection lost")); },
    }) }));
    await assert.rejects(disconnected.listen(key, () => {}).done, (error) => error.code === "network_error");
});

test("HTTP error response is canceled", async () => {
    let canceled = false;
    const client = proxy(async () => ({ ok: false, status: 503,
                                       body: new ReadableStream({ cancel() { canceled = true; } }) }));
    await assert.rejects(client.get(key), (error) => error.code === "http_error");
    assert.equal(canceled, true);
});

test("get timeout cancels a stalled stream", async () => {
    let canceled = false;
    const client = proxy(async () => ({ ok: true, status: 200,
                                       body: new ReadableStream({ cancel() { canceled = true; } }) }),
                         { timeoutMs: 20 });
    await assert.rejects(client.get(key), (error) => error.code === "timeout");
    assert.equal(canceled, true);
});

test("put reports a timeout if response cleanup stalls", async () => {
    const client = proxy(async () => ({ ok: true, status: 200,
                                       body: { cancel: () => new Promise(() => {}) } }),
                         { timeoutMs: 20 });
    await assert.rejects(client.put(key, Uint8Array.of(1)), (error) => error.code === "timeout");
});

test("listen times out before headers but stays open after headers", async () => {
    const stalled = proxy((_url, init) => new Promise((_resolve, reject) => {
        init.signal.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")), { once: true });
    }), { timeoutMs: 20 });
    await assert.rejects(stalled.listen(key, () => {}).done, (error) => error.code === "timeout");
    const open = proxy(async () => ({ ok: true, status: 200, body: new ReadableStream({}) }), { timeoutMs: 20 });
    const subscription = open.listen(key, () => {});
    await new Promise((resolve) => setTimeout(resolve, 40));
    subscription.close();
    await subscription.done;
});

test("listen readiness reports headers and failed startup", async () => {
    let resolveFetch;
    const client = proxy(() => new Promise((resolve) => { resolveFetch = resolve; }));
    const subscription = client.listen(key, () => {});
    let ready = false;
    void subscription.ready.then(() => { ready = true; });
    await Promise.resolve();
    assert.equal(ready, false);
    resolveFetch({ ok: true, status: 200, body: new ReadableStream({}) });
    assert.equal(await subscription.ready, true);
    subscription.close(); await subscription.done;
    const failed = proxy(async () => response([], 503)).listen(key, () => {});
    const failure = assert.rejects(failed.done, (error) => error.code === "http_error");
    assert.equal(await failed.ready, false);
    await failure;
});

test("aborted requests and callback failures are propagated", async () => {
    const controller = new AbortController();
    controller.abort();
    const client = proxy(async (_url, init) => { init.signal.throwIfAborted(); return response([]); });
    await assert.rejects(client.get(key, controller.signal), (error) => error.name === "AbortError");
    const streaming = proxy(async () => response([`${record("AQ==")}\n`]));
    await assert.rejects(streaming.listen(key, () => { throw new Error("handler failed"); }).done,
                         /handler failed/);
});
