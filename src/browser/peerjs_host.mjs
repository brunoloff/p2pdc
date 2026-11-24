// What the host of one key keeps (see docs/DISCOVERY_BACKENDS.md): its values until they
// expire, and who watches it. Anyone may put values, so both are capped.
const maxRecords = 256, maxRecordBytes = 1024 * 1024, maxWatchers = 256;
export const watchLeaseMs = 90000; // watchers renew every 30 s

export class KeyHost {
    constructor() {
        this.records = new Map(); // id -> { data, expires }
        this.watchers = new Map(); // "client sub" -> { client, sub, expires }
        this.listeners = new Set(); // this store's own watches: (id, data) => void
        this.bytes = 0;
    }
    sweep(now = Date.now()) {
        for (const [id, record] of this.records) if (record.expires <= now) { this.records.delete(id); this.bytes -= record.data.length; }
        for (const [name, watcher] of this.watchers) if (watcher.expires <= now) this.watchers.delete(name);
    }
    // Keeps a value (base64 data) and returns the remote watchers to tell, or null when full.
    put(id, data, lifetimeMs) {
        this.sweep();
        const previous = this.records.get(id);
        const bytes = this.bytes - (previous?.data.length ?? 0) + data.length;
        if ((!previous && this.records.size >= maxRecords) || bytes > maxRecordBytes) return null;
        this.records.set(id, { data, expires: Date.now() + lifetimeMs });
        this.bytes = bytes;
        for (const listener of this.listeners) listener(id, data);
        return [...this.watchers.values()];
    }
    values() { this.sweep(); return [...this.records].map(([id, record]) => ({ id, data: record.data })); }
    watch(client, sub) {
        this.sweep();
        const name = `${client} ${sub}`;
        if (!this.watchers.has(name) && this.watchers.size >= maxWatchers) return false;
        this.watchers.set(name, { client, sub, expires: Date.now() + watchLeaseMs });
        return true;
    }
    unwatch(client, sub) { this.watchers.delete(`${client} ${sub}`); }
    forget(client) { for (const [name, watcher] of this.watchers) if (watcher.client === client) this.watchers.delete(name); }
    get idle() { this.sweep(); return this.records.size === 0 && this.watchers.size === 0 && this.listeners.size === 0; }
}
