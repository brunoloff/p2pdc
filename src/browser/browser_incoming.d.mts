import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer } from "./wasm_discovery.mjs";
import type { SecureModule } from "./wasm_secure_link.mjs";
import type { BrowserWatch } from "./browser_watch.mjs";
import type { BrowserListenerOptions } from "./browser_listener.mjs";
import type { BrowserConnectionState } from "./browser_connection.mjs";
import type { BrowserRoute, BrowserSendResult, BrowserTransportOptions } from "./webrtc_transport.mjs";
import type { OpenDhtProxy } from "./opendht_proxy.mjs";

export type BrowserIncomingState = "new" | "starting" | "listening" | "pending" | BrowserConnectionState;
export interface BrowserIncomingOptions {
    transport: Omit<BrowserTransportOptions, "role">;
    timeoutMs?: number;
    /** Local application receive limit, 1..4194304 bytes; default 262144. */
    maxMessageBytes?: number;
    now?: () => number;
    listener?: BrowserListenerOptions;
}
export interface BrowserIncomingHandlers {
    onPublished(): void;
    onRequest(peer: DiscoveredPeer): void;
    onState(state: BrowserIncomingState): void;
    onRoute(route: BrowserRoute): void;
    onMessage(message: Uint8Array): void;
    onWritable(): void;
    onStatus(status: "retrying" | "signalingUnavailable"): void;
    onError(reason: string): void;
}
/** Experimental single incoming attempt. The application must authorize the
 * request, call retry() on discovery changes, and keep the watch alive until
 * acceptance. reject() closes locally; it does not send a rejection record. */
export class BrowserIncoming {
    constructor(module: DiscoveryModule & SecureModule, proxy: OpenDhtProxy, identity: DiscoveryOptions,
                watch: BrowserWatch, options: BrowserIncomingOptions, handlers: BrowserIncomingHandlers);
    readonly state: BrowserIncomingState;
    start(): Promise<boolean>;
    retry(): boolean;
    /** False during discovery recovery leaves the request pending until it expires. */
    accept(): Promise<boolean>;
    reject(): boolean;
    send(bytes: Uint8Array): BrowserSendResult;
    maxMessageSize(): number | null;
    route(): BrowserRoute;
    pollRoute(): Promise<BrowserRoute>;
    close(): void;
}
