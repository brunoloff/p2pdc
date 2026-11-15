import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer } from "./wasm_discovery.mjs";
import type { SecureModule } from "./wasm_secure_link.mjs";
import type { BrowserWatch } from "./browser_watch.mjs";
import type { LeaseOptions } from "./browser_lease.mjs";
import type { OpenDhtProxy } from "./opendht_proxy.mjs";
import type { BrowserConnectionState } from "./browser_connection.mjs";
import type { BrowserRoute, BrowserSendResult, BrowserTransportOptions } from "./webrtc_transport.mjs";

export type BrowserOutgoingState = "new" | "starting" | BrowserConnectionState;
export interface BrowserOutgoingOptions {
    transport: Omit<BrowserTransportOptions, "role">;
    /** Handshake timeout after publication; publication has its own bounded lifetime. */
    timeoutMs?: number;
    /** Local application receive limit, 1..4194304 bytes; default 262144. */
    maxMessageBytes?: number;
    now?: () => number;
    lease?: LeaseOptions;
}
export interface BrowserOutgoingHandlers {
    onState(state: BrowserOutgoingState): void;
    onRoute(route: BrowserRoute): void;
    onMessage(message: Uint8Array): void;
    onWritable(): void;
    onStatus(status: "retrying" | "signalingUnavailable"): void;
    onError(reason: string): void;
}
/** Experimental single attempt. Caller owns the watch and authorizes the peer.
 * start() establishes local setup, not remote delivery or authenticated readiness.
 * Accepted sends are locally queued; close suppresses undelivered callbacks. */
export class BrowserOutgoing {
    constructor(module: DiscoveryModule & SecureModule, proxy: OpenDhtProxy, identity: DiscoveryOptions,
                watch: BrowserWatch, peer: DiscoveredPeer, options: BrowserOutgoingOptions,
                handlers: BrowserOutgoingHandlers);
    readonly state: BrowserOutgoingState;
    start(): Promise<boolean>;
    send(bytes: Uint8Array): BrowserSendResult;
    maxMessageSize(): number | null;
    route(): BrowserRoute;
    pollRoute(): Promise<BrowserRoute>;
    close(): void;
}
