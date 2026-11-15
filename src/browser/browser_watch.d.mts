import type { OpenDhtProxy } from "./opendht_proxy.mjs";
import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer, DiscoveryEvent } from "./wasm_discovery.mjs";
/** Timer callbacks must be asynchronous, serialized, and invoked at most once. */
export interface DiscoveryTimers { set(callback: () => void, delayMs: number): unknown; clear(handle: unknown): void; }
export interface WatchOptions {
    sweepIntervalMs?: number; now?: () => number; timers?: DiscoveryTimers;
    recovery?: { maxRetries?: number; retryDelayMs?: number };
    /** Rotating keys are the default; false selects legacy fixed-key discovery. */ windowed?: boolean;
}
export type DiscoveryStatus = "reconnecting" | "ready";
export interface WatchHandlers {
    onPeer(peer: DiscoveredPeer, event: DiscoveryEvent): void;
    /** Lookup ingestion completed; queued peer notifications may still follow. */
    onCatchup(): void;
    onError(reason: string): void;
    onStatus?(status: DiscoveryStatus): void;
}
/** Verified group advertisements require separate application authorization. */
export class BrowserWatch {
    constructor(module: DiscoveryModule, proxy: OpenDhtProxy, identity: DiscoveryOptions,
                handlers: WatchHandlers, options?: WatchOptions);
    start(): Promise<boolean>;
    current(handle: number): DiscoveredPeer | null;
    healthy(): boolean;
    recovering(): boolean;
    close(): void;
}
