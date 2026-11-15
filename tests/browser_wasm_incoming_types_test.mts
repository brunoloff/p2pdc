import { WasmIncoming } from "../src/browser/wasm_incoming.mjs";
import type { BrowserLease } from "../src/browser/browser_lease.mjs";
import type { BrowserWatch } from "../src/browser/browser_watch.mjs";
declare const lease: BrowserLease, watch: BrowserWatch;
const incoming = new WasmIncoming(lease, watch);
const result = incoming.receive(new Uint8Array());
if (result.status === "admitted") { const handle: number = result.peer.handle; void handle; }
// @ts-expect-error Signaling must be binary.
incoming.receive("offer");
