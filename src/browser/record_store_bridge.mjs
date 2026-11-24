import { proxyFromUrls } from "./failover_proxy.mjs";
import { isTransientProxyError, keyHex } from "./proxy_codec.mjs";
import { SharedProxyWatch } from "./shared_proxy_watch.mjs";

// Event types are shared with browser_record_store.cpp. The bridge owns only
// browser operations; C++ owns callbacks, record limits and handle lifetimes.
const PUT_DONE = 0, LOOKUP_RECORD = 1, LOOKUP_DONE = 2, WATCH_READY = 3, WATCH_RECORD = 4, WATCH_ERROR = 5;
const errorCode = (error) => error?.code === "resource_limit" ? 3 :
    ["invalid_record", "invalid_stream"].includes(error?.code) ? 4 : isTransientProxyError(error) ? 2 : 6;

class RecordBridge {
    constructor(createProxy, deliver) {
        if (typeof createProxy !== "function" || typeof deliver !== "function") throw new TypeError("invalid bridge");
        this.createProxy = createProxy; this.deliver = deliver; this.stores = new Map(); this.next = 1;
        this.watches = new SharedProxyWatch();
    }
    create(url, limits) {
        if (this.next > 0xffffffff) throw new RangeError("store identifiers exhausted");
        // Several proxies arrive newline-separated; watch streams count against the first's origin.
        const proxy = this.createProxy(url, limits), id = this.next++;
        this.stores.set(id, { proxy, origin: new URL(url.split("\n")[0]).origin,
                              identity: JSON.stringify([url, limits]), operations: new Map() });
        return id;
    }
    begin(id, operation, key) {
        const store = this.stores.get(id);
        if (!store || !Number.isSafeInteger(operation) || operation < 1 || store.operations.has(operation) ||
            store.operations.size >= 128)
            throw new TypeError("invalid store operation");
        if (!(key instanceof Uint8Array) || key.length !== 20 || !key.some((value) => value !== 0))
            throw new TypeError("invalid store key");
        const entry = { id, operation, key: key.slice(), controller: new AbortController(), active: true, subscription: null };
        store.operations.set(operation, entry);
        return { store, entry };
    }
    emit(entry, kind, bytes = null, detail = 0) {
        if (!entry.active) return false;
        try { if (this.deliver(entry.id, entry.operation, kind, bytes, detail)) return true; }
        catch (error) { console.error("p2pdc: record bridge delivery failed", error); }
        this.cancel(entry.id, entry.operation); return false;
    }
    finish(store, entry) { entry.active = false; store.operations.delete(entry.operation); }
    put(id, operation, key, value) {
        if (!(value instanceof Uint8Array)) throw new TypeError("invalid record");
        const copy = value.slice(), { store, entry } = this.begin(id, operation, key);
        try {
            void store.proxy.put(entry.key, copy, entry.controller.signal).then(() => {
                if (this.emit(entry, PUT_DONE, null, 1)) this.finish(store, entry);
            }, () => { if (this.emit(entry, PUT_DONE, null, 0)) this.finish(store, entry); });
        } catch (error) { this.cancel(id, operation); throw error; }
    }
    lookup(id, operation, key, deduplicate) {
        const { store, entry } = this.begin(id, operation, key);
        try {
            void store.proxy.get(entry.key, entry.controller.signal, { deduplicateByContent: deduplicate }).then((records) => {
                for (const record of records) {
                    if (!record.expired && !this.emit(entry, LOOKUP_RECORD, record.bytes)) return;
                }
                if (this.emit(entry, LOOKUP_DONE, null, 0)) this.finish(store, entry);
            }, (error) => {
                if (!entry.active) return;
                console.error("p2pdc: proxy lookup failed", error);
                if (this.emit(entry, LOOKUP_DONE, null, errorCode(error))) this.finish(store, entry);
            });
        } catch (error) { this.cancel(id, operation); throw error; }
    }
    watch(id, operation, key) {
        const { store, entry } = this.begin(id, operation, key);
        try {
            entry.subscription = this.watches.listen(`${store.identity}:${keyHex(entry.key)}`, store.origin, store.proxy, entry.key, (record) => {
                this.emit(entry, WATCH_RECORD, record.bytes, record.expired ? 1 : 0);
            });
            let registered = false;
            void entry.subscription.ready.then((ready) => {
                if (ready && entry.active) {
                    registered = true;
                    this.emit(entry, WATCH_READY, null, 1);
                } else if (!ready) {
                    queueMicrotask(() => {
                        if (this.emit(entry, WATCH_READY, null, 0)) this.cancel(id, operation);
                    });
                }
            });
            void entry.subscription.done.then(() => {
                if (!entry.active) return;
                if (this.emit(entry, registered ? WATCH_ERROR : WATCH_READY, null, registered ? 2 : 0))
                    this.cancel(id, operation);
            }, (error) => {
                if (!entry.active) return;
                console.error("p2pdc: proxy watch failed", error);
                if (this.emit(entry, WATCH_ERROR, null, errorCode(error))) this.cancel(id, operation);
            });
        } catch (error) { this.cancel(id, operation); throw error; }
    }
    cancel(id, operation) {
        const store = this.stores.get(id), entry = store?.operations.get(operation);
        if (!entry) return;
        this.finish(store, entry); entry.controller.abort(); entry.subscription?.close();
    }
    close(id) {
        const store = this.stores.get(id);
        if (!store) return;
        for (const operation of store.operations.keys()) this.cancel(id, operation);
        this.stores.delete(id);
    }
}

export function createRecordBridge(deliver, createProxy = proxyFromUrls) {
    return new RecordBridge(createProxy, deliver);
}
