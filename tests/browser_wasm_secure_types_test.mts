import { WasmSecureLink, type SecureLinkHandlers, type SecureLinkOptions, type SecureModule } from "../src/browser/wasm_secure_link.mjs";

declare const module: SecureModule;
declare const options: SecureLinkOptions;
declare const handlers: SecureLinkHandlers;
const link = new WasmSecureLink(module, options, handlers);
link.localDescription(new Uint8Array([1]), 100);
link.localCandidate(new Uint8Array([1]), new Uint8Array([2]), 100);
link.send(new Uint8Array([3]));
// @ts-expect-error messages must be binary
link.send("text");
// @ts-expect-error roles are fixed
const invalid: SecureLinkOptions = { ...options, role: "unknown" };
void invalid;
