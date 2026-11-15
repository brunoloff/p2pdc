import { BrowserListener, type BrowserListenerHandlers } from "../src/browser/browser_listener.mjs";
import type { DiscoveryModule, DiscoveryOptions } from "../src/browser/wasm_discovery.mjs";
import type { BrowserWatch } from "../src/browser/browser_watch.mjs";
import type { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
declare const module: DiscoveryModule, proxy: OpenDhtProxy, identity: DiscoveryOptions, watch: BrowserWatch;
const handlers: BrowserListenerHandlers = { onPublished: () => {}, onRequest: () => {}, onStatus: () => {}, onError: () => {} };
const listener = new BrowserListener(module, proxy, identity, watch, handlers);
const accepted = listener.accept((record) => { const value: Uint8Array = record; void value; });
if (accepted) { const offer: Uint8Array = accepted.offer; void offer; }
// @ts-expect-error Accept requires a record callback.
listener.accept("yes");
