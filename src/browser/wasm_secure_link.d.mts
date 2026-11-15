import type { BrowserSendResult } from "./webrtc_transport.mjs";

export interface SecureModule {
    HEAPU8: Uint8Array;
    _malloc(size: number): number;
    _free(pointer: number): void;
    _p2pdc_secure_create(pointer: number, size: number): number;
    _p2pdc_secure_destroy(id: number): void;
    _p2pdc_secure_description(id: number, pointer: number, size: number, now: number): number;
    _p2pdc_secure_candidate(id: number, candidate: number, candidateSize: number,
                            mid: number, midSize: number, now: number): number;
    _p2pdc_secure_receive(id: number, pointer: number, size: number, now: number): number;
    _p2pdc_secure_description_done(id: number, success: number): number;
    _p2pdc_secure_candidate_done(id: number, operation: number, success: number): number;
    _p2pdc_secure_ready(id: number, ready: number): number;
    _p2pdc_secure_frame(id: number, pointer: number, size: number): number;
    _p2pdc_secure_send(id: number, pointer: number, size: number): number;
    _p2pdc_secure_wake(id: number): number;
    _p2pdc_secure_timeout(id: number): number;
}
export interface SecureLinkOptions {
    role: "offerer" | "answerer";
    identitySeed: Uint8Array;
    recipientSecret: Uint8Array;
    capability: Uint8Array;
    remoteSigningKey: Uint8Array;
    remoteBoxKey: Uint8Array;
    localInstance: Uint8Array;
    remoteInstance: Uint8Array;
    attemptId: Uint8Array;
    timeoutMs: number;
    /** Local application receive limit, 1..4194304 bytes; default 262144. */
    maxMessageBytes?: number;
}
export type FlowResult = "accepted" | "ignored" | "failed" | "closed";
export interface SecureLinkHandlers {
    sendFrame(frame: Uint8Array): BrowserSendResult;
    bufferedAmount(): number | null;
    onSignal(wire: Uint8Array): void | Promise<void>;
    onDescription(sdp: Uint8Array): void | Promise<void>;
    onCandidate(candidate: Uint8Array, mid: Uint8Array): void | Promise<void>;
    onReady(maximum: number): void | Promise<void>;
    onMessage(message: Uint8Array): void | Promise<void>;
    onWritable(): void | Promise<void>;
    onFailure(reason: string): void;
    onControl(kind: number): void | Promise<void>;
}
/** Internal preauthorized attempt. Inputs remain caller-owned; close on failure. */
export class WasmSecureLink {
    constructor(module: SecureModule, options: SecureLinkOptions, handlers: SecureLinkHandlers);
    localDescription(bytes: Uint8Array, now: number): FlowResult;
    localCandidate(candidate: Uint8Array, mid: Uint8Array, now: number): FlowResult;
    receive(bytes: Uint8Array, now: number): FlowResult;
    setTransportReady(ready: boolean, maxMessageSize?: number | null): boolean;
    onFrame(bytes: Uint8Array): boolean;
    send(bytes: Uint8Array): BrowserSendResult;
    wake(): void;
    timeout(): void;
    close(): void;
}
