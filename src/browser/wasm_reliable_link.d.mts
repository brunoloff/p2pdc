import type { BrowserSendResult } from "./webrtc_transport.mjs";

export interface ReliableModule {
    HEAPU8: Uint8Array;
    _malloc(size: number): number;
    _free(pointer: number): void;
    _p2pdc_reliable_create(): number;
    _p2pdc_reliable_destroy(id: number): void;
    _p2pdc_reliable_ready(id: number, ready: number): void;
    _p2pdc_reliable_wake(id: number): void;
    _p2pdc_reliable_send(id: number, pointer: number, size: number): number;
    _p2pdc_reliable_frame(id: number, pointer: number, size: number): void;
}
export interface ReliableHandlers {
    sendFrame(frame: Uint8Array): BrowserSendResult;
    bufferedAmount(): number | null;
    onMessage(message: Uint8Array): void;
    onFailure(reason: string): void;
    onWritable(): void;
}
/** Experimental unauthenticated framing; callbacks must not reenter during send. */
export class WasmReliableLink {
    constructor(module: ReliableModule, handlers: ReliableHandlers);
    setReady(ready: boolean, maxMessageSize?: number | null): boolean;
    wake(): void;
    send(bytes: Uint8Array): BrowserSendResult;
    onFrame(bytes: Uint8Array): boolean;
    close(): void;
}
