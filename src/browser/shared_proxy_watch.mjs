import { PollingProxyWatch, ProxyWatchCache } from "./polling_proxy_watch.mjs";
import { FallbackProxyWatch } from "./fallback_proxy_watch.mjs";

// One stream may serve multiple independent watches of the same key and limits.
// Callers still perform a separate catch-up lookup after their registration.
export class SharedProxyWatch {
    constructor(maxStreams = 2, interval = 750, cacheLimit = 16 * 1024 * 1024) {
        if (!Number.isSafeInteger(maxStreams) || maxStreams < 0 ||
            !Number.isSafeInteger(interval) || interval <= 0 ||
            !Number.isSafeInteger(cacheLimit) || cacheLimit <= 0) throw new TypeError("invalid watch limits");
        this.groups = new Map(); this.streams = new Map();
        this.maxStreams = maxStreams; this.interval = interval; this.cache = new ProxyWatchCache(cacheLimit);
    }
    listen(identity, origin, proxy, key, onRecord) {
        let group = this.groups.get(identity);
        if (!group) group = this.create(identity, origin, proxy, key);
        const member = { onRecord, active: true };
        group.members.add(member);
        return { ready: group.subscription.ready, done: group.subscription.done,
                 close: () => this.remove(group, member) };
    }
    create(identity, origin, proxy, key) {
        // A null origin is a backend that multiplexes watches, with no connection budget.
        const group = { identity, origin, members: new Set(), controller: new AbortController(), subscription: null,
                        streamed: origin === null || (this.streams.get(origin) || 0) < this.maxStreams };
        const emit = (record) => {
            for (const member of [...group.members])
                if (member.active) member.onRecord(record);
        };
        group.subscription = group.streamed ? new FallbackProxyWatch(proxy, key, emit, this.cache, this.interval, group.controller.signal) :
            new PollingProxyWatch(proxy, key, emit, this.cache, this.interval);
        if (group.streamed && origin !== null) this.streams.set(origin, (this.streams.get(origin) || 0) + 1);
        this.groups.set(identity, group);
        return group;
    }
    remove(group, member) {
        if (!member.active) return;
        member.active = false;
        group.members.delete(member);
        if (group.members.size) return;
        if (this.groups.get(group.identity) === group) this.groups.delete(group.identity);
        if (group.streamed && group.origin !== null) {
            const remaining = this.streams.get(group.origin) - 1;
            if (remaining) this.streams.set(group.origin, remaining); else this.streams.delete(group.origin);
        }
        group.controller.abort();
        group.subscription.close();
    }
}
