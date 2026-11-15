import type { SecureModule, SecureLinkOptions, FlowResult } from "./wasm_secure_link.mjs";
import type { BrowserTransportOptions, BrowserRoute, BrowserSendResult } from "./webrtc_transport.mjs";

export type BrowserConnectionState = "connecting" | "ready" | "suspended" | "failed" | "closed";
export interface BrowserConnectionOptions {
    secure: SecureLinkOptions;
    transport: BrowserTransportOptions;
    /** Validated wall time in whole Unix seconds; regressions fail the attempt. */
    now?: () => number;
}
export interface BrowserConnectionHandlers {
    /** Admit the record to an ordered signaling channel; reject or throw on admission failure. */
    onSignal(wire: Uint8Array): void | Promise<void>;
    onState(state: BrowserConnectionState): void;
    onRoute(route: BrowserRoute): void;
    onMessage(message: Uint8Array): void;
    onWritable(): void;
    onError(reason: string): void;
}
/** Internal preauthorized connection; the caller owns discovery, consent and signaling delivery.
 * The deadline starts at construction. Other notification handlers must be synchronous.
 * start() completes local startup, not authentication. Accepted sends are locally queued. */
export class BrowserConnection {
    constructor(module: SecureModule, options: BrowserConnectionOptions, handlers: BrowserConnectionHandlers);
    readonly state: BrowserConnectionState;
    start(): Promise<void>;
    receive(wire: Uint8Array): FlowResult;
    send(bytes: Uint8Array): BrowserSendResult;
    /** Negotiated application message size, or null before authentication or after failure/close. */
    maxMessageSize(): number | null;
    route(): BrowserRoute;
    pollRoute(): Promise<BrowserRoute>;
    close(): void;
}
