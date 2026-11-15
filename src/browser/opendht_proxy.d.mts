export interface ProxyRecord {
    bytes: Uint8Array;
    id: string | null;
    expired: boolean;
}
export interface ProxyOptions {
    maxValueBytes?: number;
    maxValues?: number;
    maxTotalBytes?: number;
    timeoutMs?: number;
    fetch?: typeof fetch;
}
export interface ProxySubscription {
    /** True means response headers arrived; it does not prove DHT registration. */
    readonly ready: Promise<boolean>;
    readonly done: Promise<void>;
    close(): void;
}
export class ProxyError extends Error {
    constructor(code: string, message: string);
    readonly code: string;
    readonly status?: number;
}
/** Experimental browser proxy transport. Returned records are not authenticated. */
export class OpenDhtProxy {
    constructor(url: string | URL, options?: ProxyOptions);
    put(key: Uint8Array, value: Uint8Array, signal?: AbortSignal): Promise<void>;
    get(key: Uint8Array, signal?: AbortSignal,
        options?: { deduplicateByContent?: boolean }): Promise<ProxyRecord[]>;
    /** Callbacks are synchronous; unexpected stream closure rejects done. */
    listen(key: Uint8Array, onRecord: (record: ProxyRecord) => void,
           signal?: AbortSignal): ProxySubscription;
}
