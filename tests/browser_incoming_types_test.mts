import { BrowserIncoming, type BrowserIncomingHandlers, type BrowserIncomingOptions }
    from "../src/browser/browser_incoming.mjs";
import type { DiscoveryModule, DiscoveryOptions } from "../src/browser/wasm_discovery.mjs";
import type { SecureModule } from "../src/browser/wasm_secure_link.mjs";
import type { BrowserWatch } from "../src/browser/browser_watch.mjs";
import type { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
declare const module: DiscoveryModule & SecureModule, proxy: OpenDhtProxy, identity: DiscoveryOptions;
declare const watch: BrowserWatch, options: BrowserIncomingOptions;
const smallOptions: BrowserIncomingOptions = { transport: { relayPolicy: "automatic" }, maxMessageBytes: 8192 };
const handlers: BrowserIncomingHandlers = { onPublished: () => {}, onRequest: () => {}, onState: () => {},
    onRoute: () => {}, onMessage: () => {}, onWritable: () => {}, onStatus: () => {}, onError: () => {} };
const owner = new BrowserIncoming(module, proxy, identity, watch, options, handlers);
const accepted: Promise<boolean> = owner.accept();
// @ts-expect-error Messages must be binary.
owner.send("text");
void accepted;
void smallOptions;
