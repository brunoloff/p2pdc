export interface DiscoveryModule {
    HEAPU8: Uint8Array;
    _malloc(size: number): number;
    _free(pointer: number): void;
    _p2pdc_discovery_create(pointer: number, size: number): number;
    _p2pdc_discovery_destroy(id: number): void;
    _p2pdc_discovery_healthy(id: number): number;
    _p2pdc_discovery_key(id: number, pointer: number, size: number): number;
    _p2pdc_discovery_window_key?(id: number, now: number, pointer: number, size: number): number;
    _p2pdc_discovery_self(id: number, pointer: number, size: number): number;
    _p2pdc_discovery_publish(id: number, now: number, pointer: number, size: number): number;
    _p2pdc_discovery_publish_windowed?(id: number, now: number, pointer: number, size: number): number;
    _p2pdc_discovery_receive(id: number, pointer: number, size: number, now: number): number;
    _p2pdc_discovery_sweep(id: number, now: number): number;
    _p2pdc_discovery_current(id: number, handle: number, now: number, pointer: number, size: number): number;
}
export interface DiscoveryOptions { identitySeed: Uint8Array; recipientSecret: Uint8Array; capability: Uint8Array; }
export interface LocalEndpoint { signingKey: Uint8Array; recipientKey: Uint8Array; instance: Uint8Array; inbox: Uint8Array; }
export interface DiscoveredPeer extends LocalEndpoint {
    handle: number; sequence: bigint; features: bigint; issuedAt: bigint; expiresAt: bigint;
}
export type DiscoveryEvent = "seen" | "updated" | "expired";
export interface DiscoveryHandlers { onPeer(peer: DiscoveredPeer, event: DiscoveryEvent): void; onError(reason: string): void; }
/** Updated peers have new handles. Replace old entries using signingKey + instance. */
export class WasmDiscovery {
    constructor(module: DiscoveryModule, options: DiscoveryOptions, handlers: DiscoveryHandlers);
    key(): Uint8Array | null;
    windowKey(now: number): Uint8Array | null;
    self(): LocalEndpoint | null;
    publish(now: number): Uint8Array | null;
    publishWindowed(now: number): Uint8Array | null;
    receive(wire: Uint8Array, now: number): "changed" | "ignored" | "failed" | "closed";
    sweep(now: number): boolean;
    current(handle: number, now: number): DiscoveredPeer | null;
    close(): void;
}
