import { WasmDiscovery, type DiscoveryModule, type DiscoveryOptions, type DiscoveryHandlers } from "../src/browser/wasm_discovery.mjs";
declare const module: DiscoveryModule;
declare const options: DiscoveryOptions;
declare const handlers: DiscoveryHandlers;
const discovery = new WasmDiscovery(module, options, handlers);
const peer = discovery.current(1, 100);
if (peer) { const sequence: bigint = peer.sequence; void sequence; }
// @ts-expect-error records must be binary
discovery.receive("not binary", 100);
