import test from "node:test";
import assert from "node:assert/strict";
import { FailoverProxy } from "../src/browser/failover_proxy.mjs";
import { ProxyError } from "../src/browser/proxy_codec.mjs";

const key = Uint8Array.from({ length: 20 }, (_, index) => index + 1);
const httpError = (status) => Object.assign(new ProxyError("http_error", `HTTP ${status}`), { status });

// A proxy whose calls answer from a queue of outcomes: an Error is thrown, anything else returned.
function scripted(name, outcomes = []) {
    const calls = [];
    const next = async (method) => {
        calls.push(method);
        const outcome = outcomes.length ? outcomes.shift() : `${name}-ok`;
        if (outcome instanceof Error) throw outcome;
        return outcome;
    };
    return {
        calls,
        put: () => next("put"),
        get: () => next("get"),
        listen: () => {
            calls.push("listen");
            const outcome = outcomes.shift();
            return { ready: Promise.resolve(!(outcome instanceof Error)),
                     done: outcome instanceof Error ? Promise.reject(outcome) : new Promise(() => {}),
                     close() {} };
        },
    };
}

test("uses the first proxy while it works", async () => {
    const first = scripted("first"), second = scripted("second");
    const proxy = new FailoverProxy([first, second]);
    assert.equal(await proxy.get(key), "first-ok");
    assert.equal(await proxy.put(key, Uint8Array.of(1)), "first-ok");
    assert.deepEqual(second.calls, []);
});

test("moves on when a proxy is unreachable, times out or is overloaded, and stays there", async () => {
    for (const failure of [new ProxyError("network_error", "refused"), new ProxyError("timeout", "slow"),
                           httpError(503), httpError(429)]) {
        const first = scripted("first", [failure]), second = scripted("second");
        const proxy = new FailoverProxy([first, second]);
        assert.equal(await proxy.put(key, Uint8Array.of(1)), "second-ok");
        assert.equal(await proxy.get(key), "second-ok");
        assert.deepEqual(first.calls, ["put"]);
        assert.deepEqual(second.calls, ["put", "get"]);
    }
});

test("does not move on for errors another proxy would repeat", async () => {
    const refused = httpError(400), first = scripted("first", [refused]), second = scripted("second");
    const proxy = new FailoverProxy([first, second]);
    await assert.rejects(proxy.get(key), refused);
    assert.deepEqual(second.calls, []);
    assert.equal(await proxy.get(key), "first-ok");
});

test("gives each proxy one try, then reports the last failure", async () => {
    const down = () => new ProxyError("network_error", "refused");
    const last = new ProxyError("timeout", "slow");
    const proxy = new FailoverProxy([scripted("a", [down()]), scripted("b", [down()]), scripted("c", [last])]);
    await assert.rejects(proxy.get(key), last);
    assert.equal(await proxy.get(key), "a-ok"); // round again, from the first
});

test("an aborted request is not tried elsewhere", async () => {
    const controller = new AbortController(), second = scripted("second");
    const first = { put: async () => { controller.abort(); throw new ProxyError("network_error", "aborted"); } };
    const proxy = new FailoverProxy([first, second]);
    await assert.rejects(proxy.put(key, Uint8Array.of(1), controller.signal));
    assert.deepEqual(second.calls, []);
});

test("a failed watch sends the next watch to the next proxy", async () => {
    const first = scripted("first", [new ProxyError("network_error", "refused")]), second = scripted("second", [null]);
    const proxy = new FailoverProxy([first, second]);
    const failing = proxy.listen(key, () => {});
    await assert.rejects(failing.done);
    await Promise.resolve();
    assert.equal(await proxy.listen(key, () => {}).ready, true);
    assert.deepEqual([first.calls, second.calls], [["listen"], ["listen"]]);
});

test("an ended watch stream does not move on", async () => {
    const first = scripted("first", [new ProxyError("stream_closed", "ended"), null]), second = scripted("second");
    const proxy = new FailoverProxy([first, second]);
    await assert.rejects(proxy.listen(key, () => {}).done);
    await Promise.resolve();
    proxy.listen(key, () => {});
    assert.deepEqual(second.calls, []);
});

test("a failover proxy needs between one and eight proxies", () => {
    assert.throws(() => new FailoverProxy([]), TypeError);
    assert.throws(() => new FailoverProxy(Array(9).fill(scripted("x"))), TypeError);
    assert.doesNotThrow(() => new FailoverProxy([scripted("x")]));
});
