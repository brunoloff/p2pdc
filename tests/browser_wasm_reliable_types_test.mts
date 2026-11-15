import { WasmReliableLink, type ReliableHandlers, type ReliableModule } from "../src/browser/wasm_reliable_link.mjs";

declare const module: ReliableModule;
declare const handlers: ReliableHandlers;
const link = new WasmReliableLink(module, handlers);
link.setReady(true, 4096);
link.send(new Uint8Array([1, 2]));
link.onFrame(new Uint8Array([3]));
// @ts-expect-error messages must be binary
link.send("text");
// @ts-expect-error transport limits must be numeric
link.setReady(true, "4096");
