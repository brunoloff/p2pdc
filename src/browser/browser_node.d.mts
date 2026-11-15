import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer, DiscoveryEvent } from "./wasm_discovery.mjs";
import type { SecureModule } from "./wasm_secure_link.mjs";
import type { WatchOptions, DiscoveryStatus } from "./browser_watch.mjs";
import type { OpenDhtProxy } from "./opendht_proxy.mjs";
import type { BrowserOutgoing, BrowserOutgoingOptions, BrowserOutgoingHandlers } from "./browser_outgoing.mjs";
import type { BrowserIncoming, BrowserIncomingOptions, BrowserIncomingHandlers } from "./browser_incoming.mjs";

export type BrowserNodeState = "new" | "starting" | "running" | "failed" | "closed";
export interface BrowserNodeHandlers {
    onPeer(peer: DiscoveredPeer, event: DiscoveryEvent): void;
    onCatchup(): void;
    onError(reason: string): void;
    onStatus?(status: DiscoveryStatus): void;
}
export type BrowserOutgoingHandle = Pick<BrowserOutgoing, "state" | "send" | "maxMessageSize" | "route" | "pollRoute" | "close">;
export type BrowserIncomingHandle = Pick<BrowserIncoming, "state" | "accept" | "reject" | "send" | "maxMessageSize" | "route" | "pollRoute" | "close">;
/** Experimental single-capability node. Discovery verifies identity, not authorization.
 * Rotating discovery is the default; watch.windowed=false selects the legacy protocol.
 * Only peer objects emitted by this node can be passed to connect(). If discovery
 * fails, pending attempts close and authenticated connections remain open until
 * explicitly closed or node.close(). All callbacks run on the browser event loop. */
export class BrowserNode {
    constructor(module: DiscoveryModule & SecureModule, proxy: OpenDhtProxy, identity: DiscoveryOptions,
                handlers: BrowserNodeHandlers, options?: { watch?: WatchOptions });
    readonly state: BrowserNodeState;
    start(): Promise<boolean>;
    connect(peer: DiscoveredPeer, options: BrowserOutgoingOptions, handlers: BrowserOutgoingHandlers): BrowserOutgoingHandle;
    listen(options: BrowserIncomingOptions, handlers: BrowserIncomingHandlers): BrowserIncomingHandle;
    close(): void;
}
