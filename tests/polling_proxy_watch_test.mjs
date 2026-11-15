import test from "node:test";
import assert from "node:assert/strict";
import { PollingProxyWatch, ProxyWatchCache } from "../src/browser/polling_proxy_watch.mjs";
import { SharedProxyWatch } from "../src/browser/shared_proxy_watch.mjs";
import { FallbackProxyWatch } from "../src/browser/fallback_proxy_watch.mjs";
import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const record = (id, value, expired = false) => ({ id, bytes: Uint8Array.of(value), expired });
async function until(condition) {
    for (let index = 0; index < 100 && !condition(); ++index) await delay(5);
    assert.ok(condition(), "timed out waiting for poll");
}

test("polling delivers changes once, tolerates incomplete snapshots and releases its cache", async () => {
    let snapshot = [record("a", 1)], calls = 0;
    const proxy = { async get(_key, _signal, options) {
        assert.equal(options.deduplicateByContent, true); ++calls; return snapshot;
    } };
    const seen = [], cache = new ProxyWatchCache(1024);
    const watch = new PollingProxyWatch(proxy, Uint8Array.of(1), (value) => seen.push(value), cache, 5);
    assert.equal(await watch.ready, true);
    await until(() => calls >= 3);
    assert.equal(seen.length, 1);
    snapshot = [record("a", 2), record("b", 3, true)];
    await until(() => seen.length === 3);
    assert.deepEqual(seen.map((value) => [value.id, value.bytes[0], value.expired]),
        [["a", 1, false], ["a", 2, false], ["b", 3, true]]);
    snapshot = [];
    await until(() => cache.used === 0);
    assert.equal(seen.length, 3);
    watch.close(); await watch.done;
    assert.equal(cache.used, 0);
    const stoppedAt = calls; await delay(15); assert.equal(calls, stoppedAt);
});

test("polling reports startup errors and rejects a snapshot exceeding the cache limit", async () => {
    const error = new Error("lookup failed");
    const failed = new PollingProxyWatch({ get: async () => { throw error; } }, Uint8Array.of(1),
                                         () => {}, new ProxyWatchCache(1024), 5);
    const failure = assert.rejects(failed.done, error);
    assert.equal(await failed.ready, false); await failure;
    const cache = new ProxyWatchCache(1);
    const full = new PollingProxyWatch({ get: async () => [record("a", 1)] }, Uint8Array.of(1),
                                       () => assert.fail("over-budget record delivered"), cache, 5);
    const overflow = assert.rejects(full.done, (value) => value.code === "resource_limit");
    await overflow; assert.equal(cache.used, 0);
});

test("polling propagates a later failure and cancels an in-flight lookup", async () => {
    let calls = 0;
    const cache = new ProxyWatchCache(1024);
    const watch = new PollingProxyWatch({ async get() {
        if (++calls === 1) return [record("a", 1)];
        throw new Error("later lookup failed");
    } }, Uint8Array.of(1), () => {}, cache, 5);
    const failure = assert.rejects(watch.done, /later lookup failed/);
    assert.equal(await watch.ready, true); await failure; assert.equal(cache.used, 0);
    let signal;
    const waiting = new PollingProxyWatch({ get(_key, value) {
        signal = value;
        return new Promise((_resolve, reject) => value.addEventListener("abort", () => reject(new Error("aborted"))));
    } }, Uint8Array.of(2), () => assert.fail("closed watch delivered"), cache, 5);
    waiting.close();
    assert.equal(signal.aborted, true); assert.equal(await waiting.ready, false);
    await waiting.done; assert.equal(cache.used, 0);
});

test("stream budget is per origin and overflow watches poll and cancel independently", async () => {
    const streams = [], polls = [], pool = new SharedProxyWatch(1, 5, 1024);
    const proxy = {
        listen(_key, _emit, signal) {
            const stream = { signal, closed: false }; streams.push(stream);
            return { ready: Promise.resolve(true), done: new Promise(() => {}),
                     close: () => { stream.closed = true; } };
        },
        async get(key) { polls.push(key[0]); return [record("v", key[0])]; },
    };
    const first = pool.listen("first", "https://one", proxy, Uint8Array.of(1), () => {});
    const received = [];
    const second = pool.listen("second", "https://one", proxy, Uint8Array.of(2), (value) => received.push(value));
    const third = pool.listen("third", "https://two", proxy, Uint8Array.of(3), () => {});
    assert.equal(await second.ready, true);
    await until(() => received.length === 1);
    assert.equal(streams.length, 2); assert.deepEqual(polls, [2]);
    second.close(); await second.done;
    first.close();
    const replacement = pool.listen("replacement", "https://one", proxy, Uint8Array.of(4), () => {});
    assert.equal(streams.length, 3);
    replacement.close(); third.close();
    assert.ok(streams.every((stream) => stream.closed && stream.signal.aborted));
    assert.equal(pool.cache.used, 0); assert.equal(pool.groups.size, 0);
});

test("startup stream timeout falls back to bounded polling", async () => {
    const seen = [], cache = new ProxyWatchCache(1024);
    const proxy = {
        listen() { return { ready: Promise.resolve(false),
            done: Promise.reject(Object.assign(new Error("timeout"), { code: "timeout" })), close() {} }; },
        async get() { return [record("a", 1)]; },
    };
    const watch = new FallbackProxyWatch(proxy, Uint8Array.of(1), (value) => seen.push(value), cache, 5);
    assert.equal(await watch.ready, true);
    await until(() => seen.length === 1);
    watch.close(); await watch.done; assert.equal(cache.used, 0);
});

test("a real proxy startup timeout falls back before the longer watch deadline", async () => {
    let polls = 0;
    const proxy = new OpenDhtProxy("https://proxy.example", {
        timeoutMs: 10,
        fetch(url, init) {
            if (url.endsWith("/listen")) return new Promise((_resolve, reject) => {
                init.signal.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")), { once: true });
            });
            ++polls;
            return Promise.resolve({ ok: true, status: 200,
                body: new ReadableStream({ start(controller) { controller.close(); } }) });
        },
    });
    const watch = new FallbackProxyWatch(proxy, new Uint8Array(20).fill(1), () => {},
                                         new ProxyWatchCache(1024), 50, undefined, 200);
    const failure = watch.done.catch((error) => error);
    assert.equal(await watch.ready, true);
    assert.ok(polls >= 1);
    watch.close();
    assert.equal(await failure, undefined);
});

test("quiet startup falls back and closing a pending stream cancels its deadline", async () => {
    let closed = 0, calls = 0;
    const pending = new Promise(() => {});
    const proxy = { listen: () => ({ ready: pending, done: pending, close: () => { ++closed; } }),
                    get: async () => { ++calls; return []; } };
    const cache = new ProxyWatchCache(1024);
    const watch = new FallbackProxyWatch(proxy, Uint8Array.of(1), () => {}, cache, 50, undefined, 5);
    assert.equal(await watch.ready, true); assert.equal(calls, 1); assert.equal(closed, 1);
    watch.close(); await watch.done; assert.equal(cache.used, 0);
    const canceled = new FallbackProxyWatch(proxy, Uint8Array.of(2), () => {}, cache, 50, undefined, 5);
    canceled.close(); assert.equal(await canceled.ready, false); await canceled.done;
    await delay(10); assert.equal(calls, 1);
});

test("a failed fallback lookup reports the error and does not become ready", async () => {
    const pending = new Promise(() => {}), error = new Error("poll failed");
    const proxy = { listen: () => ({ ready: pending, done: pending, close() {} }),
                    get: async () => { throw error; } };
    const watch = new FallbackProxyWatch(proxy, Uint8Array.of(1), () => {}, new ProxyWatchCache(1024),
                                         5, undefined, 5);
    const failure = assert.rejects(watch.done, error);
    assert.equal(await watch.ready, false); await failure;
});

test("non-timeout startup and later stream failures remain errors", async () => {
    for (const started of [false, true]) {
        const error = Object.assign(new Error("network failure"), { code: "network_error" });
        let reject;
        const done = new Promise((_resolve, fail) => { reject = fail; });
        const proxy = { listen: () => ({ ready: Promise.resolve(started), done, close() {} }),
                        get: () => assert.fail("unexpected polling") };
        const watch = new FallbackProxyWatch(proxy, Uint8Array.of(1), () => {}, new ProxyWatchCache(1024), 5);
        const failure = assert.rejects(watch.done, error);
        if (started) assert.equal(await watch.ready, true);
        reject(error);
        await failure; assert.equal(await watch.ready, started);
    }
});

test("even a stream rejection without an error value is propagated", async () => {
    const proxy = { listen: () => ({ ready: new Promise(() => {}), done: Promise.reject(undefined), close() {} }) };
    const watch = new FallbackProxyWatch(proxy, Uint8Array.of(1), () => {}, new ProxyWatchCache(1024), 5);
    await assert.rejects(watch.done);
    assert.equal(await watch.ready, false);
});
