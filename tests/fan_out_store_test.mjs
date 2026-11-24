import test from "node:test";
import assert from "node:assert/strict";
import { FanOutStore } from "../src/browser/fan_out_store.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";
import { storeLimits } from "../src/browser/store_limits.mjs";

const key = Uint8Array.from({ length: 20 }, (_, index) => index + 1);
const limits = storeLimits();
const record = (byte, id) => ({ bytes: Uint8Array.of(byte), expired: false, id });
const down = () => new ProxyError("network_error", "unreachable");
const never = (signal) => new Promise((_resolve, reject) =>
    signal?.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")), { once: true }));

// A backend whose operations answer from functions; listen calls are counted.
function backend({ put = async () => {}, get = async () => [], listen } = {}) {
    const fake = { puts: 0, listens: 0, closed: false, close() { this.closed = true; } };
    fake.put = (...args) => { ++fake.puts; return put(...args); };
    fake.get = get;
    fake.listen = (...args) => { ++fake.listens; return listen(...args); };
    return fake;
}
const watching = (onStart) => (k, onRecord) => {
    let close;
    const done = new Promise((resolve) => { close = resolve; });
    onStart?.(onRecord);
    return { ready: Promise.resolve(true), done, close };
};
const failing = () => () => ({ ready: Promise.resolve(false), done: Promise.reject(down()), close() {} });

test("a put reaches every backend and succeeds when one does", async () => {
    const good = backend(), bad = backend({ put: async () => { throw down(); } });
    await new FanOutStore([bad, good], limits).put(key, Uint8Array.of(1));
    assert.deepEqual([bad.puts, good.puts], [1, 1]);
    const error = await new FanOutStore([bad, backend({ put: async () => { throw down(); } })], limits)
        .put(key, Uint8Array.of(1)).catch((failure) => failure);
    assert.equal(error.code, "network_error");
});

test("a lookup merges every backend's values, keeping values apart by backend", async () => {
    const store = new FanOutStore([backend({ get: async () => [record(1, "a"), record(2, "b")] }),
                                   backend({ get: async () => [record(1, "a")] })], limits);
    const merged = await store.get(key);
    assert.deepEqual(merged.map((value) => `${value.bytes[0]}@${value.id}`), ["1@0:a", "2@0:b", "1@1:a"]);
    const byContent = await store.get(key, undefined, { deduplicateByContent: true });
    assert.deepEqual(byContent.map((value) => value.bytes[0]), [1, 2]);
});

test("a blocked backend holds a lookup up only for the grace period", async () => {
    const controller = new AbortController();
    const store = new FanOutStore([backend({ get: async () => [record(7, "x")] }),
                                   backend({ get: (k, signal) => never(controller.signal) })], limits);
    const started = Date.now();
    assert.deepEqual((await store.get(key)).map((value) => value.bytes[0]), [7]);
    assert.ok(Date.now() - started < 2500, "answered within the grace period");
    controller.abort();
});

test("a lookup fails only when every backend fails", async () => {
    const store = new FanOutStore([backend({ get: async () => { throw down(); } }),
                                   backend({ get: async () => { throw down(); } })], limits);
    assert.equal((await store.get(key).catch((error) => error)).code, "network_error");
});

test("a watch hears every backend, and starts a failed one again while the others carry on", async () => {
    let deliver;
    const steady = backend({ listen: watching((onRecord) => { deliver = onRecord; }) });
    let attempts = 0;
    const flaky = backend({ listen: (...args) => (++attempts === 1 ? failing()() : watching()(...args)) });
    const heard = [];
    const watch = new FanOutStore([steady, flaky], limits).listen(key, (value) => heard.push(value.id));
    assert.equal(await watch.ready, true);
    deliver(record(1, "v"));
    assert.deepEqual(heard, ["0:v"]);
    await new Promise((resolve) => setTimeout(resolve, 2300)); // the first backoff is 2 s
    assert.equal(flaky.listens, 2);
    watch.close();
    await watch.done;
});

test("a watch gives up when every backend fails before any is ready", async () => {
    const watch = new FanOutStore([backend({ listen: failing() }), backend({ listen: failing() })], limits)
        .listen(key, () => {});
    assert.equal(await watch.ready, false);
    assert.equal((await watch.done.catch((error) => error)).code, "network_error");
});

test("closing the store closes every backend", () => {
    const first = backend(), second = backend();
    new FanOutStore([first, second], limits).close();
    assert.deepEqual([first.closed, second.closed], [true, true]);
    assert.throws(() => new FanOutStore([first], limits), TypeError);
});
