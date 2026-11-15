import type { OpenDhtProxy } from "./opendht_proxy.mjs";
import type { DiscoveryModule, DiscoveryOptions, LocalEndpoint } from "./wasm_discovery.mjs";
import type { DiscoveryTimers } from "./browser_watch.mjs";
export interface LeaseOptions { retryMs?: number; maxPublications?: number; now?: () => number; timers?: DiscoveryTimers;
    /** Rotating keys are the default; false selects legacy fixed-key discovery. */ windowed?: boolean; }
export type LeaseError = "retrying" | "publicationLimit" | "clock" | "discovery" | "resource" |
                         "expiredPending" | "callback" | "scheduler" | "backend";
export interface LeaseHandlers { onPublished(): void; onError(reason: LeaseError): void; }
/** Start only after the owner is prepared to receive on the advertised inbox. */
export class BrowserLease {
    constructor(module: DiscoveryModule, proxy: OpenDhtProxy, identity: DiscoveryOptions,
                handlers: LeaseHandlers, options?: LeaseOptions);
    start(): boolean;
    /** The owner must open this inbox before starting publication. */
    endpoint(): LocalEndpoint | null;
    self(): LocalEndpoint | null;
    healthy(): boolean;
    close(): void;
}
