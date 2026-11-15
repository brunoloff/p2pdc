import type { BrowserLease } from "./browser_lease.mjs";
import type { BrowserWatch } from "./browser_watch.mjs";
import type { DiscoveredPeer } from "./wasm_discovery.mjs";

export type IncomingAdmission = { status: "closed" | "failed" | "notReady" | "ignored" | "unknownPeer" | "nonOffer" | "otherOffer" } |
    { status: "admitted"; peer: DiscoveredPeer; attemptId: Uint8Array; wire: Uint8Array };
/** Internal verification only; the caller must obtain consent and recheck the lease and peer. */
export class WasmIncoming {
    constructor(lease: BrowserLease, watch: BrowserWatch, options?: { now?: () => number });
    receive(wire: Uint8Array, inspectOnly?: boolean): IncomingAdmission;
    close(): void;
}
