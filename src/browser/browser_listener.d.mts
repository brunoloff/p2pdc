import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer, LocalEndpoint } from "./wasm_discovery.mjs";
import type { BrowserWatch } from "./browser_watch.mjs";
import type { LeaseOptions } from "./browser_lease.mjs";
import type { OpenDhtProxy } from "./opendht_proxy.mjs";

export type BrowserListenerState = "new" | "starting" | "listening" | "pending" | "accepted" | "failed" | "closed";
export interface BrowserListenerHandlers {
    onPublished(): void;
    onRequest(peer: DiscoveredPeer): void;
    onStatus(reason: "retrying"): void;
    onError(reason: string, detail?: unknown): void;
}
export interface BrowserListenerOptions extends LeaseOptions {
    requestTimeoutMs?: number;
    /** Nondecreasing Unix milliseconds for enforcing the consent deadline. */
    deadlineNow?: () => number;
}
export interface BrowserAcceptedOffer {
    peer: DiscoveredPeer;
    local: LocalEndpoint;
    attemptId: Uint8Array;
    offer: Uint8Array;
}
/** Internal single-use listener. Consent is explicit and close rejects pending requests.
 * Start the authenticated connection and feed it the accepted offer, then call
 * resume() to release queued records. Keep this listener alive for signaling;
 * retry after discovery changes. */
export class BrowserListener {
    constructor(module: DiscoveryModule, proxy: OpenDhtProxy, identity: DiscoveryOptions,
                watch: BrowserWatch, handlers: BrowserListenerHandlers, options?: BrowserListenerOptions);
    readonly state: BrowserListenerState;
    start(): Promise<boolean>;
    retry(): boolean;
    accept(onRecord: (wire: Uint8Array) => void): BrowserAcceptedOffer | null;
    resume(): boolean;
    close(): void;
}
