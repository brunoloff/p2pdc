import { OpenDhtProxy } from "./opendht_proxy.mjs";
import { FailoverProxy } from "./failover_proxy.mjs";
import { FanOutStore } from "./fan_out_store.mjs";
import { NostrRelay } from "./nostr_relay.mjs";
import { MqttBroker } from "./mqtt_broker.mjs";
import { PeerJsSwitchboard } from "./peerjs_switchboard.mjs";
import { storeLimits } from "./store_limits.mjs";

const maxBackends = 8, maxProxies = 8; // as discovery_config.cpp allows
// WebSocket clients, shared by every store that uses the same server: p2pdc makes a store per
// space and per connection attempt, and servers frown on many connections from one browser.
const clients = new Map(); // name -> { client, users }

// A store over the shared client for name, with its own limits; closing it releases the client.
function sharedClient(name, create, limits) {
    let entry = clients.get(name);
    if (!entry) { entry = { client: create(), users: 0 }; clients.set(name, entry); }
    ++entry.users;
    let released = false;
    return {
        streamOrigin: null,
        put: (key, value, signal) => entry.client.put(key, value, signal, limits),
        get: (key, signal, options) => entry.client.get(key, signal, options, limits),
        listen: (key, onRecord, signal) => entry.client.listen(key, onRecord, signal, limits),
        close() {
            if (released) return;
            released = true;
            if (--entry.users > 0) return;
            clients.delete(name);
            entry.client.close();
        },
    };
}

function stringField(entry, name) {
    if (typeof entry[name] !== "string" || !entry[name]) throw new TypeError(`backend ${name} must be a string`);
    return entry[name];
}

function openDhtStore(entry, limits) {
    if (!Array.isArray(entry.urls) || entry.urls.length < 1 || entry.urls.length > maxProxies)
        throw new TypeError(`OpenDHT backends take 1 to ${maxProxies} proxy URLs`);
    const proxies = entry.urls.map((url) => new OpenDhtProxy(url, limits));
    const store = proxies.length === 1 ? proxies[0] : new FailoverProxy(proxies);
    // Each watch is a long HTTP request, and browsers allow few per origin.
    store.streamOrigin = new URL(entry.urls[0]).origin;
    return store;
}

// One meeting place from its configuration entry (see docs/DISCOVERY_BACKENDS.md).
export function createBackend(entry, limits) {
    if (!entry || typeof entry !== "object" || Array.isArray(entry)) throw new TypeError("invalid backend entry");
    switch (entry.kind) {
    case "opendht": return openDhtStore(entry, limits);
    case "nostr": {
        const url = stringField(entry, "url");
        return sharedClient(`nostr ${url}`, () => new NostrRelay(url, limits), limits);
    }
    case "mqtt": {
        const url = stringField(entry, "url");
        return sharedClient(`mqtt ${url}`, () => new MqttBroker(url, limits), limits);
    }
    case "peerjs": {
        const url = stringField(entry, "url"), key = stringField(entry, "key");
        return sharedClient(`peerjs ${url} ${key}`, () => new PeerJsSwitchboard(url, key, limits), limits);
    }
    default: throw new TypeError("unknown discovery backend kind");
    }
}

// The record store for a JSON list of backend entries, as browser_record_store.cpp passes it:
// one backend as it is, several used all at once.
export function createBackendStore(text, options = {}) {
    let entries;
    try { entries = JSON.parse(text); } catch { throw new TypeError("backend list must be JSON"); }
    if (!Array.isArray(entries) || entries.length < 1 || entries.length > maxBackends)
        throw new TypeError(`between 1 and ${maxBackends} discovery backends are required`);
    const limits = storeLimits(options);
    const backends = entries.map((entry) => createBackend(entry, limits));
    return backends.length === 1 ? backends[0] : new FanOutStore(backends, limits);
}
