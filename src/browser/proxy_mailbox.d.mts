import type { OpenDhtProxy } from "./opendht_proxy.mjs";
export interface MailboxOptions { maxQueuedRecords?: number; maxQueuedBytes?: number; maxRecordBytes?: number;
    /** Also collapses identical records currently awaiting delivery. */
    deduplicateByContent?: boolean; }
export interface MailboxHandlers {
    onRecord(record: Uint8Array): void;
    onCatchup(): void;
    onError(error: unknown): void;
}
/** Raw records may be stale, duplicated or untrusted. Stream headers are not a registration guarantee. */
export class ProxyMailbox {
    constructor(proxy: OpenDhtProxy, key: Uint8Array, handlers: MailboxHandlers, options?: MailboxOptions);
    start(): Promise<boolean>;
    healthy(): boolean;
    close(): void;
}
