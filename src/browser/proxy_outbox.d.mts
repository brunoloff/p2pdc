import type { OpenDhtProxy } from "./opendht_proxy.mjs";
export interface OutboxOptions { maxQueuedRecords?: number; maxQueuedBytes?: number; maxRecordBytes?: number; }
export interface OutboxHandlers { onPublished(): void; onError(error: unknown): void; }
export type OutboxResult = "accepted" | "tooLarge" | "backpressure" | "failed" | "closed";
/** Retains a failed publication until retry; successful publication does not imply remote delivery. */
export class ProxyOutbox {
    constructor(proxy: OpenDhtProxy, key: Uint8Array, handlers: OutboxHandlers, options?: OutboxOptions);
    send(record: Uint8Array): OutboxResult;
    retry(): boolean;
    healthy(): boolean;
    close(): void;
}
