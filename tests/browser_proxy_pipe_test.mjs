import test from "node:test";
import assert from "node:assert/strict";
import { ProxyMailbox } from "../src/browser/proxy_mailbox.mjs";
import { ProxyOutbox } from "../src/browser/proxy_outbox.mjs";

const key = Uint8Array.from({ length: 20 }, (_, i) => i + 1);
function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
function fixture() {
    const opened = deferred(), finished = deferred(), lookup = deferred(), writes = [];
    let callback, closed = false, lookups = 0;
    const proxy = {
        listen: (_key, onRecord) => { callback = onRecord; return { ready: opened.promise, done: finished.promise,
            close: () => { closed = true; finished.resolve(); } }; },
        get: () => { ++lookups; return lookup.promise; },
        put: (_key, value) => { const pending = deferred(); writes.push({ value: [...value], ...pending }); return pending.promise; },
    };
    const seen = { records: [], errors: [], catchups: 0, published: 0 };
    const mailboxHandlers = { onRecord: (value) => seen.records.push([...value]),
        onError: (error) => seen.errors.push(error.message), onCatchup: () => ++seen.catchups };
    const outboxHandlers = { onPublished: () => ++seen.published, onError: (error) => seen.errors.push(error.message) };
    return { opened, finished, lookup, writes, proxy, seen, mailboxHandlers, outboxHandlers,
        record: (bytes, expired = false) => callback({ bytes: Uint8Array.from(bytes), expired }),
        get closed() { return closed; }, get lookups() { return lookups; } };
}
async function settle() { for (let i = 0; i < 8; ++i) await Promise.resolve(); }

test("mailbox opens listen before lookup and preserves overlap and expiration", async () => {
    const f = fixture();
    const mailbox = new ProxyMailbox(f.proxy, key, f.mailboxHandlers);
    const started = mailbox.start();
    assert.equal(f.lookups, 0);
    f.record([1]); f.record([9], true);
    f.opened.resolve(true); await settle();
    assert.equal(f.lookups, 1);
    f.lookup.resolve([{ bytes: Uint8Array.of(1), expired: false }, { bytes: Uint8Array.of(2), expired: false }]);
    assert.equal(await started, true); await settle();
    assert.deepEqual(f.seen.records, [[1], [1], [2]]);
    assert.equal(f.seen.catchups, 1);
    mailbox.close(); assert.equal(f.closed, true);
    assert.equal(mailbox.healthy(), false);
});

test("mailbox failures and close suppress later delivery", async () => {
    const f = fixture();
    const mailbox = new ProxyMailbox(f.proxy, key, f.mailboxHandlers, { maxQueuedRecords: 1 });
    const started = mailbox.start();
    f.record([1]); f.record([2]); await settle();
    assert.deepEqual(f.seen.records, []);
    assert.deepEqual(f.seen.errors, ["mailbox receive limit exceeded"]);
    f.opened.resolve(false);
    assert.equal(await started, false);
    const g = fixture();
    const other = new ProxyMailbox(g.proxy, key, g.mailboxHandlers);
    const pending = other.start();
    g.opened.resolve(true); await settle();
    other.close(); g.lookup.resolve([{ bytes: Uint8Array.of(3), expired: false }]);
    assert.equal(await pending, false); await settle();
    assert.deepEqual(g.seen.records, []);
});

test("discovery mailbox collapses queued copies but still limits distinct records", async () => {
    const f = fixture();
    const mailbox = new ProxyMailbox(f.proxy, key, f.mailboxHandlers,
        { maxQueuedRecords: 1, deduplicateByContent: true });
    const started = mailbox.start();
    for (let i = 0; i < 150; ++i) f.record([1]);
    f.opened.resolve(true); await settle(); f.lookup.resolve([]);
    assert.equal(await started, true); await settle();
    assert.deepEqual(f.seen.records, [[1]]);
    f.record([2]); f.record([3]); await settle();
    assert.deepEqual(f.seen.errors, ["mailbox receive limit exceeded"]);
    mailbox.close();
    const g = fixture(), plain = new ProxyMailbox(g.proxy, key, g.mailboxHandlers, { maxQueuedRecords: 1 });
    const pending = plain.start(); g.record([1]); g.record([1]); await settle(); g.opened.resolve(false);
    assert.equal(await pending, false);
    assert.deepEqual(g.seen.errors, ["mailbox receive limit exceeded"]);
    plain.close();
});

test("larger discovery mailbox drains a populated lookup without dropping records", async () => {
    const f = fixture();
    const mailbox = new ProxyMailbox(f.proxy, key, f.mailboxHandlers,
        { maxQueuedRecords: 512, deduplicateByContent: true });
    const started = mailbox.start(); f.opened.resolve(true); await settle();
    f.lookup.resolve(Array.from({ length: 200 }, (_, index) =>
        ({ bytes: Uint8Array.of(index >> 8, index & 255), expired: false })));
    assert.equal(await started, true);
    for (let i = 0; i < 30; ++i) await Promise.resolve();
    assert.equal(f.seen.records.length, 200); assert.deepEqual(f.seen.errors, []); mailbox.close();
});

test("mailbox reports stream and lookup failures", async () => {
    const f = fixture();
    const mailbox = new ProxyMailbox(f.proxy, key, f.mailboxHandlers);
    const pending = mailbox.start();
    f.opened.resolve(true); await settle();
    f.lookup.reject(new Error("lookup failed"));
    assert.equal(await pending, false); await settle();
    assert.deepEqual(f.seen.errors, ["lookup failed"]);
    const g = fixture();
    const other = new ProxyMailbox(g.proxy, key, g.mailboxHandlers);
    const started = other.start();
    g.opened.resolve(true); g.lookup.resolve([]);
    assert.equal(await started, true);
    g.finished.reject(new Error("stream failed")); await settle();
    assert.deepEqual(g.seen.errors, ["stream failed"]);
});

test("outbox serializes, retains failed bytes, and retries without mutation", async () => {
    const f = fixture();
    const outbox = new ProxyOutbox(f.proxy, key, f.outboxHandlers, { maxQueuedRecords: 2, maxQueuedBytes: 4, maxRecordBytes: 4 });
    const input = Uint8Array.of(1, 2);
    assert.equal(outbox.send(input), "accepted"); input[0] = 9;
    assert.equal(outbox.send(Uint8Array.of(3, 4)), "accepted");
    assert.equal(outbox.send(Uint8Array.of(5)), "backpressure");
    await settle(); assert.deepEqual(f.writes[0].value, [1, 2]); assert.equal(f.writes.length, 1);
    f.writes[0].reject(new Error("offline")); await settle();
    assert.deepEqual(f.seen.errors, ["offline"]); assert.equal(outbox.retry(), true);
    await settle(); assert.deepEqual(f.writes[1].value, [1, 2]);
    f.writes[1].resolve(); await settle();
    assert.deepEqual(f.writes[2].value, [3, 4]);
    f.writes[2].resolve(); await settle();
    assert.equal(f.seen.published, 2);
    assert.equal(outbox.retry(), false);
    outbox.close();
});

test("outbox enforces limits and suppresses completion after close", async () => {
    const f = fixture();
    const outbox = new ProxyOutbox(f.proxy, key, f.outboxHandlers, { maxQueuedBytes: 4, maxRecordBytes: 4 });
    assert.equal(outbox.send(new Uint8Array(5)), "tooLarge");
    assert.throws(() => outbox.send("bad"), TypeError);
    assert.equal(outbox.send(Uint8Array.of(1)), "accepted"); await settle();
    outbox.close(); f.writes[0].resolve(); await settle();
    assert.equal(f.seen.published, 0);
    assert.equal(outbox.send(Uint8Array.of(2)), "closed");
});

test("outbox retry must occur after the error callback returns", async () => {
    const f = fixture();
    let outbox, inlineRetry;
    f.outboxHandlers.onError = () => { inlineRetry = outbox.retry(); };
    outbox = new ProxyOutbox(f.proxy, key, f.outboxHandlers);
    assert.equal(outbox.send(Uint8Array.of(1)), "accepted"); await settle();
    f.writes[0].reject(new Error("offline")); await settle();
    assert.equal(inlineRetry, false);
    assert.equal(outbox.retry(), true); await settle();
    f.writes[1].resolve(); await settle();
    outbox.close();
});

test("mailbox validates callbacks, record sizes, and close before readiness", async () => {
    const f = fixture();
    assert.throws(() => new ProxyMailbox(f.proxy, new Uint8Array(20), f.mailboxHandlers), TypeError);
    f.mailboxHandlers.onRecord = () => { throw new Error("consumer failed"); };
    const mailbox = new ProxyMailbox(f.proxy, key, f.mailboxHandlers);
    const pending = mailbox.start(); f.record([1]); await settle();
    assert.deepEqual(f.seen.errors, ["consumer failed"]);
    f.opened.resolve(false); assert.equal(await pending, false);
    const g = fixture();
    const small = new ProxyMailbox(g.proxy, key, g.mailboxHandlers, { maxQueuedBytes: 2, maxRecordBytes: 2 });
    const started = small.start(); g.record([1, 2, 3]); await settle();
    assert.deepEqual(g.seen.errors, ["mailbox receive limit exceeded"]);
    g.opened.resolve(false); assert.equal(await started, false);
    const h = fixture();
    const closed = new ProxyMailbox(h.proxy, key, h.mailboxHandlers);
    const waiting = closed.start(); closed.close(); h.opened.resolve(false);
    assert.equal(await waiting, false); assert.equal(h.lookups, 0);
});

test("synchronous publication failure retains the record for later retry", async () => {
    const f = fixture();
    let fail = true;
    f.proxy.put = () => { if (fail) throw new Error("offline"); return Promise.resolve(); };
    const outbox = new ProxyOutbox(f.proxy, key, f.outboxHandlers);
    assert.equal(outbox.send(Uint8Array.of(1)), "accepted"); await settle();
    assert.deepEqual(f.seen.errors, ["offline"]);
    fail = false; assert.equal(outbox.retry(), true); await settle();
    assert.equal(f.seen.published, 1); outbox.close();
});
