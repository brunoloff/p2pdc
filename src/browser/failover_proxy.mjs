import { isTransientProxyError } from "./proxy_codec.mjs";

const maxFailoverProxies = 8; // as discovery_config.cpp allows

// A failure another proxy may not have: unreachable, timed out, or HTTP 408, 429 or 5xx. A
// stream the proxy ended is not one; watchers reconnect to the same proxy.
function switchable(error) {
    return isTransientProxyError(error) && error.code !== "stream_closed";
}

// Several OpenDHT proxies that reach the same DHT, used one at a time in the order given.
// Requests go to the current proxy. When one fails in a way another proxy could fix, the
// next proxy becomes current, and a store or lookup is tried again there, until each proxy
// has had one try. A watch stays on the proxy it started on; when it fails that way, the
// next watch starts on the next proxy. The proxy that works stays current.
export class FailoverProxy {
    constructor(proxies) {
        if (!Array.isArray(proxies) || proxies.length < 1 || proxies.length > maxFailoverProxies)
            throw new TypeError(`between 1 and ${maxFailoverProxies} proxies are required`);
        this.proxies = proxies.slice();
        this.current = 0;
    }
    close() { for (const proxy of this.proxies) proxy.close?.(); }
    failed(index, error) {
        if (index === this.current && switchable(error)) this.current = (index + 1) % this.proxies.length;
    }
    async attempt(call, signal) {
        let last;
        for (let tried = 0; tried < this.proxies.length; ++tried) {
            const index = this.current;
            try {
                return await call(this.proxies[index]);
            } catch (error) {
                if (signal?.aborted || !switchable(error)) throw error;
                this.failed(index, error);
                last = error;
            }
        }
        throw last;
    }
    put(key, value, signal) { return this.attempt((proxy) => proxy.put(key, value, signal), signal); }
    get(key, signal, options) { return this.attempt((proxy) => proxy.get(key, signal, options), signal); }
    listen(key, onRecord, signal) {
        const index = this.current, subscription = this.proxies[index].listen(key, onRecord, signal);
        subscription.done.catch((error) => this.failed(index, error));
        return subscription;
    }
}
